#ifndef CLOSED_LOOP_RULE_ENGINE_HPP
#define CLOSED_LOOP_RULE_ENGINE_HPP

#include "core/Types.hpp"
#include <vector>
#include <memory>
#include <optional>

// 前向声明YAML节点
namespace YAML {
    class Node;
}

namespace clma {

class RuleEngine {
public:
    RuleEngine();
    ~RuleEngine();
    
    // 从YAML文件加载规则
    bool loadRulesFromFile(const std::string& filepath);
    
    // 从YAML字符串加载规则
    bool loadRulesFromString(const std::string& yaml_content);
    
    // 查找匹配的规则
    std::vector<Rule> findMatchingRules(const std::string& query) const;
    
    // 获取最佳匹配规则（基于权重或优先级）
    std::optional<Rule> getBestRule(const std::string& query) const;
    
    // 获取所有规则
    const std::vector<Rule>& getAllRules() const { return rules_; }
    
    // 添加规则
    void addRule(const Rule& rule);
    
    // 清空规则
    void clearRules() { rules_.clear(); }
    
    // 获取规则数量
    size_t ruleCount() const { return rules_.size(); }
    
private:
    std::vector<Rule> rules_;
    
    // 解析YAML节点为Rule对象
    Rule parseRuleFromYaml(const YAML::Node& node);
    
    // 计算规则匹配分数（用于排序）
    double calculateMatchScore(const Rule& rule, const std::string& query) const;
};

} // namespace clma

#endif // CLOSED_LOOP_RULE_ENGINE_HPP