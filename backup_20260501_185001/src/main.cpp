#include <iostream>
#include "core/Types.hpp"
#include "core/RuleEngine.hpp"

int main() {
    std::cout << "Closed-Loop Multi-Agent Framework Test" << std::endl;
    
    // Test basic types
    clma::EvaluationScore score{0.8, 0.7, 0.9};
    std::cout << "Overall score: " << score.overall() << std::endl;
    
    // Test rule engine
    clma::RuleEngine engine;
    std::cout << "Rule engine initialized. Rule count: " << engine.ruleCount() << std::endl;
    
    // Try to load rules
    bool loaded = engine.loadRulesFromFile("../config/rules/default.yaml");
    if (loaded) {
        std::cout << "Rules loaded successfully. Total rules: " << engine.ruleCount() << std::endl;
        
        // Test rule matching
        std::string query = "write a program to calculate fibonacci";
        auto rules = engine.findMatchingRules(query);
        std::cout << "Found " << rules.size() << " matching rules for query: " << query << std::endl;
        
        for (size_t i = 0; i < rules.size() && i < 3; ++i) {
            std::cout << "  Rule " << (i+1) << ": " << rules[i].pattern << std::endl;
        }
    } else {
        std::cout << "Failed to load rules (yaml-cpp might not be available)" << std::endl;
    }
    
    std::cout << "Test completed." << std::endl;
    return 0;
}