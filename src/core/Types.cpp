#include "core/Types.hpp"
#include <regex>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <stack>
#include <functional>

namespace clma {

bool Rule::matches(const std::string& query) const {
    if (pattern.empty()) return false;
    
    try {
        std::regex re(pattern, std::regex::icase | std::regex::optimize);
        return std::regex_search(query, re);
    } catch (const std::regex_error&) {
        // Fallback: substring match if pattern is not valid regex
        std::string query_lower = query;
        std::string pattern_lower = pattern;
        std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
        std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(), ::tolower);
        return query_lower.find(pattern_lower) != std::string::npos;
    }
}

// ==================== TaskGraph 实现 ====================

void TaskGraph::addNode(const TaskNode& node) {
    nodes.push_back(node);
}

std::vector<size_t> TaskGraph::getReadyNodeIndices() const {
    std::vector<size_t> ready;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (node.status != "pending" && node.status != "failed") {
            continue;  // 只考虑 pending 或 failed（可重试）的节点
        }
        if (node.status == "failed" && node.retry_count >= node.max_retries) {
            continue;  // 已耗尽重试次数
        }
        
        // 检查所有依赖是否已完成
        bool allDepsDone = true;
        for (const auto& depId : node.dependencies) {
            const TaskNode* dep = findNode(depId);
            if (!dep || dep->status != "done") {
                allDepsDone = false;
                break;
            }
        }
        if (allDepsDone) {
            ready.push_back(i);
        }
    }
    return ready;
}

bool TaskGraph::allCompleted() const {
    for (const auto& node : nodes) {
        if (node.status == "pending" || node.status == "running") {
            return false;
        }
    }
    return true;
}

TaskNode* TaskGraph::findNode(const std::string& id) {
    for (auto& node : nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

const TaskNode* TaskGraph::findNode(const std::string& id) const {
    for (const auto& node : nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

bool TaskGraph::hasCyclicDependency() const {
    // DFS 检测循环依赖
    std::unordered_map<std::string, int> visited; // 0=white,1=gray,2=black
    std::function<bool(const std::string&)> dfs = [&](const std::string& id) -> bool {
        auto it = visited.find(id);
        if (it != visited.end()) {
            if (it->second == 1) return true; // 发现回边
            return false;
        }
        visited[id] = 1; // gray
        const TaskNode* node = findNode(id);
        if (node) {
            for (const auto& dep : node->dependencies) {
                if (dfs(dep)) return true;
            }
        }
        visited[id] = 2; // black
        return false;
    };
    
    for (const auto& node : nodes) {
        if (dfs(node.id)) return true;
    }
    return false;
}

size_t TaskGraph::getCompletedCount() const {
    size_t count = 0;
    for (const auto& node : nodes) {
        if (node.status == "done") count++;
    }
    return count;
}

size_t TaskGraph::getFailedCount() const {
    size_t count = 0;
    for (const auto& node : nodes) {
        if (node.status == "failed") count++;
    }
    return count;
}

} // namespace clma