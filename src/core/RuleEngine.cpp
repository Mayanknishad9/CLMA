#include "core/RuleEngine.hpp"
#include <fstream>
#include <algorithm>
#include <sstream>

// 尝试包含yaml-cpp，如果不可用则使用存根
#ifdef HAS_YAML_CPP
#include <yaml-cpp/yaml.h>
#else
// 简单存根，避免编译错误
namespace YAML {
    class Node {
    public:
        template<typename T> T as() const { return T(); }
        template<typename T> T as(const T& defaultValue) const { return defaultValue; }
        bool IsSequence() const { return false; }
        bool IsMap() const { return false; }
        template<typename T> T operator[](const T&) const { return Node(); }
        bool operator==(const Node&) const { return false; }
    };
    inline Node LoadFile(const std::string&) { return Node(); }
    inline Node Load(const std::string&) { return Node(); }
}
#endif

namespace clma {

RuleEngine::RuleEngine() {
    // 初始化空规则集
}

RuleEngine::~RuleEngine() {
    // 清理资源
}

bool RuleEngine::loadRulesFromFile(const std::string& filepath) {
    try {
        YAML::Node config = YAML::LoadFile(filepath);
        if (!config["rules"]) {
            return false;
        }
        
        rules_.clear();
        for (const auto& rule_node : config["rules"]) {
            Rule rule = parseRuleFromYaml(rule_node);
            rules_.push_back(rule);
        }
        return true;
    } catch (const std::exception& e) {
        // 记录错误
        return false;
    }
}

bool RuleEngine::loadRulesFromString(const std::string& yaml_content) {
    try {
        YAML::Node config = YAML::Load(yaml_content);
        if (!config["rules"]) {
            return false;
        }
        
        rules_.clear();
        for (const auto& rule_node : config["rules"]) {
            Rule rule = parseRuleFromYaml(rule_node);
            rules_.push_back(rule);
        }
        return true;
    } catch (const std::exception& e) {
        // 记录错误
        return false;
    }
}

std::vector<Rule> RuleEngine::findMatchingRules(const std::string& query) const {
    std::vector<std::pair<double, Rule>> scored_rules;
    
    for (const auto& rule : rules_) {
        if (rule.matches(query)) {
            double score = calculateMatchScore(rule, query);
            scored_rules.emplace_back(score, rule);
        }
    }
    
    // 按分数降序排序
    std::sort(scored_rules.begin(), scored_rules.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    // 提取规则
    std::vector<Rule> result;
    for (const auto& [score, rule] : scored_rules) {
        result.push_back(rule);
    }
    
    return result;
}

std::optional<Rule> RuleEngine::getBestRule(const std::string& query) const {
    auto matches = findMatchingRules(query);
    if (matches.empty()) {
        return std::nullopt;
    }
    return matches[0];  // 最高分
}

void RuleEngine::addRule(const Rule& rule) {
    rules_.push_back(rule);
}

Rule RuleEngine::parseRuleFromYaml(const YAML::Node& node) {
    Rule rule;
    
    if (node["pattern"]) {
        rule.pattern = node["pattern"].as<std::string>();
    }
    
    if (node["validation_method"]) {
        rule.validation_method = node["validation_method"].as<std::string>();
    }
    
    if (node["recommended_tools"] && node["recommended_tools"].IsSequence()) {
        for (const auto& tool_node : node["recommended_tools"]) {
            rule.recommended_tools.push_back(tool_node.as<std::string>());
        }
    }
    
    if (node["weights"] && node["weights"].IsMap()) {
        if (node["weights"]["reasonableness"]) {
            rule.weights["reasonableness"] = node["weights"]["reasonableness"].as<double>();
        }
        if (node["weights"]["executability"]) {
            rule.weights["executability"] = node["weights"]["executability"].as<double>();
        }
        if (node["weights"]["satisfaction"]) {
            rule.weights["satisfaction"] = node["weights"]["satisfaction"].as<double>();
        }
    }
    
    if (node["threshold"]) {
        rule.threshold = node["threshold"].as<double>();
    } else {
        rule.threshold = 0.8;  // 默认阈值
    }
    
    return rule;
}

double RuleEngine::calculateMatchScore(const Rule& rule, const std::string& query) const {
    // 简单评分算法：
    // 1. 模式匹配长度比例
    // 2. 规则权重总和
    // 3. 工具数量（越多越好）
    
    double score = 0.0;
    
    // 模式匹配分数（简单实现）
    std::string query_lower = query;
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::string pattern_lower = rule.pattern;
    std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    
    if (query_lower.find(pattern_lower) != std::string::npos) {
        score += 1.0;
    }
    
    // 添加权重分数
    double weight_sum = 0.0;
    for (const auto& [key, value] : rule.weights) {
        weight_sum += value;
    }
    score += weight_sum / 3.0;  // 归一化
    
    // 工具数量加分
    score += rule.recommended_tools.size() * 0.1;
    
    return score;
}

} // namespace clma