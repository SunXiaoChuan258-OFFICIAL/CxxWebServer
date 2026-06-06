#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

// 定时器节点绑定 fd过期时间和回调
struct TimerNode {
    int fd;
    std::chrono::steady_clock::time_point expire;
    std::function<void()> callback;

    TimerNode(int fd, std::chrono::steady_clock::time_point exp, std::function<void()> cb)
        : fd(fd), expire(exp), callback(std::move(cb)) {}
};

// 时间堆定时器：小顶堆，堆顶始终为最近到期的定时器
// 只在主线程中调用，无需加锁

class TimerHeap {
public:
    TimerHeap() = default;

    // 添加定时器，若fd已存在则覆盖
    void add_timer(int fd, std::chrono::steady_clock::time_point expire,
                   std::function<void()> cb) {
        if (fd_idex_map.count(fd)) del_timer(fd);
        size_t idx = m_heap.size();
        m_heap.push_back(std::make_unique<TimerNode>(fd, expire, std::move(cb)));
        fd_idex_map[fd] = idx;
        bubble_up(idx);//新添加的根据expire上浮
    }

    // 刷新定时器，连接有活动时调用
    void adjust_timer(int fd, std::chrono::steady_clock::time_point new_expire) {
        auto it = fd_idex_map.find(fd);
        if (it == fd_idex_map.end()) return;

        size_t idx = it->second;
        auto old = m_heap[idx]->expire;
        m_heap[idx]->expire = new_expire;
        //更新超时时间，根据时间变大/变小决定上浮或下沉
        if (new_expire < old)
            bubble_up(idx);
        else if (new_expire > old)
            bubble_down(idx);
    }

    // 删除定时器（连接关闭时调用）
    void del_timer(int fd) {
        auto it = fd_idex_map.find(fd);
        if (it == fd_idex_map.end()) return;
        size_t idx = it->second;
        size_t last = m_heap.size() - 1;
        if (idx != last) {
            swap_node(idx, last);
            m_heap.pop_back();
            bubble_down(idx);
        } else {
            m_heap.pop_back();
        }
        fd_idex_map.erase(fd);//解除映射
    }

    // 执行所有已过期的定时器回调，返回触发数量
    int tick() {
        auto now = std::chrono::steady_clock::now();
        int count = 0;
        while (!m_heap.empty() && m_heap[0]->expire <= now) {
            auto& node = m_heap[0];
            fd_idex_map.erase(node->fd);
            if (node->callback) node->callback();//解映射并调用回调

            size_t last = m_heap.size() - 1;
            if (last > 0) {
                swap_node(0, last);//踢出并重新建堆
                m_heap.pop_back();
                bubble_down(0);
            } else {
                m_heap.pop_back();
            }
            ++count;
        }
        return count;
    }

    // 返回距离下一个定时器到期的毫秒数，-1表示没有定时器可无限阻塞
    int next_timeout_ms() const {
        if (m_heap.empty()) return -1;
        auto now = std::chrono::steady_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
            m_heap[0]->expire - now); //堆顶-当前时间，求最小超时等待时间，并转换成ms
        return std::max(0, static_cast<int>(diff.count()));//不能小于0
    }

    bool empty() const { return m_heap.empty(); }

private:
    std::vector<std::unique_ptr<TimerNode>> m_heap;
    std::unordered_map<int, size_t> fd_idex_map; // fd堆索引哈希

    void swap_node(size_t i, size_t j) {
        std::swap(m_heap[i], m_heap[j]);
        fd_idex_map[m_heap[i]->fd] = i;
        fd_idex_map[m_heap[j]->fd] = j;
    }

    void bubble_up(size_t idx) {//小的上浮
        while (idx > 0) {
            size_t parent = (idx - 1) / 2;
            if (m_heap[idx]->expire < m_heap[parent]->expire) {
                swap_node(idx, parent);
                idx = parent;
            } else {
                break;
            }
        }
    }

    void bubble_down(size_t idx) {//从idx开始迭代调整，大的下沉
        size_t n = m_heap.size();
        while (true) {
            size_t smallest = idx;
            size_t left = 2 * idx + 1;
            size_t right = 2 * idx + 2;
            if (left < n && m_heap[left]->expire < m_heap[smallest]->expire)
                smallest = left;
            if (right < n && m_heap[right]->expire < m_heap[smallest]->expire)
                smallest = right;
            if (smallest != idx) {
                swap_node(idx, smallest);
                idx = smallest;
            } else {
                break;//已经是小根堆了退出
            }
        }
    }
};
