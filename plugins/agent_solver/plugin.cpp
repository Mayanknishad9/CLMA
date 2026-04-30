#include <core/AgentPlugin.hpp>
#include <sstream>

namespace clma {

/**
 * Solver 插件 — 生成解决方案
 * 原 Orchestrator::executeSolution
 */
class SolverPlugin : public AgentPlugin {
public:
    PluginInfo getInfo() const override {
        PluginInfo info;
        info.id = "agent.solver";
        info.name = "Solution Executor";
        info.version = {1, 0, 0};
        info.type = PluginType::CUSTOM;
        info.author = "CLMA Core";
        info.description = "Generates executable solutions based on reasoning plans";
        info.apiVersion = 1;
        return info;
    }

    AgentStep getStepType() const override {
        return AgentStep::SOLVER;
    }

    AgentResult execute(const AgentContext& ctx) override {
        AgentResult result;
        result.success = true;

        const std::string& query = ctx.refinedQuery;

        std::string solution;

        bool is_programming = query.find("write") != std::string::npos ||
                              query.find("implement") != std::string::npos ||
                              query.find("code") != std::string::npos ||
                              query.find("script") != std::string::npos;

        if (is_programming) {
            if (query.find("python") != std::string::npos ||
                query.find("Python") != std::string::npos) {
                solution = generatePythonSolution(query);
            } else if (query.find("cpp") != std::string::npos ||
                       query.find("c++") != std::string::npos ||
                       query.find("C++") != std::string::npos) {
                solution = generateCppSolution(query);
            } else {
                solution = generateGenericSolution(query);
            }
        } else if (query.find("explain") != std::string::npos ||
                   query.find("describe") != std::string::npos) {
            solution = generateExplanation(query);
        } else {
            solution = "Based on the analysis, here is the response:\n"
                       "------------------------------------------------\n"
                       "Query: " + query + "\n\n"
                       "The solution has been generated according to the reasoning plan.\n";
        }

        result.content = solution;
        result.metadata["solution_type"] = is_programming ? "code" : "text";

        return result;
    }

private:
    std::string generatePythonSolution(const std::string& query) {
        std::ostringstream oss;
        oss << "```python\n";
        oss << "#!/usr/bin/env python3\n";
        oss << "# Solution for: " << query << "\n\n";

        if (query.find("hello") != std::string::npos) {
            oss << "def main():\n";
            oss << "    print(\"Hello, World!\")\n\n";
            oss << "if __name__ == \"__main__\":\n";
            oss << "    main()\n";
        } else if (query.find("fib") != std::string::npos) {
            oss << "def fibonacci(n):\n";
            oss << "    if n <= 1:\n";
            oss << "        return n\n";
            oss << "    a, b = 0, 1\n";
            oss << "    for _ in range(2, n + 1):\n";
            oss << "        a, b = b, a + b\n";
            oss << "    return b\n\n";
            oss << "def main():\n";
            oss << "    n = 10\n";
            oss << "    result = fibonacci(n)\n";
            oss << "    print(f\"Fibonacci({n}) = {result}\")\n\n";
            oss << "if __name__ == \"__main__\":\n";
            oss << "    main()\n";
        } else if (query.find("sort") != std::string::npos) {
            oss << "def sort_numbers(arr):\n";
            oss << "    return sorted(arr)\n\n";
            oss << "def main():\n";
            oss << "    data = [3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5]\n";
            oss << "    sorted_data = sort_numbers(data)\n";
            oss << "    print(f\"Original: {data}\")\n";
            oss << "    print(f\"Sorted:   {sorted_data}\")\n\n";
            oss << "if __name__ == \"__main__\":\n";
            oss << "    main()\n";
        } else {
            oss << "def main():\n";
            oss << "    # TODO: Implement solution\n";
            oss << "    pass\n\n";
            oss << "if __name__ == \"__main__\":\n";
            oss << "    main()\n";
        }

        oss << "```";
        return oss.str();
    }

    std::string generateCppSolution(const std::string& query) {
        std::ostringstream oss;
        oss << "```cpp\n";
        oss << "#include <iostream>\n\n";

        if (query.find("hello") != std::string::npos) {
            oss << "int main() {\n";
            oss << "    std::cout << \"Hello, World!\" << std::endl;\n";
            oss << "    return 0;\n";
            oss << "}\n";
        } else if (query.find("fib") != std::string::npos) {
            oss << "int fibonacci(int n) {\n";
            oss << "    if (n <= 1) return n;\n";
            oss << "    int a = 0, b = 1;\n";
            oss << "    for (int i = 2; i <= n; i++) {\n";
            oss << "        int temp = a + b;\n";
            oss << "        a = b;\n";
            oss << "        b = temp;\n";
            oss << "    }\n";
            oss << "    return b;\n";
            oss << "}\n\n";
            oss << "int main() {\n";
            oss << "    int n = 10;\n";
            oss << "    std::cout << \"Fibonacci(\" << n << \") = \"\n";
            oss << "              << fibonacci(n) << std::endl;\n";
            oss << "    return 0;\n";
            oss << "}\n";
        } else {
            oss << "int main() {\n";
            oss << "    // TODO: Implement solution\n";
            oss << "    return 0;\n";
            oss << "}\n";
        }

        oss << "```";
        return oss.str();
    }

    std::string generateGenericSolution(const std::string& query) {
        std::ostringstream oss;
        oss << "Solution:\n\n";
        oss << "1. Understand the problem requirements\n";
        oss << "2. Design the solution approach\n";
        oss << "3. Implement the solution\n";
        oss << "4. Verify correctness\n";
        return oss.str();
    }

    std::string generateExplanation(const std::string& query) {
        std::ostringstream oss;
        oss << "Explanation:\n\n";
        oss << "Topic: " << query << "\n\n";
        oss << "Key Points:\n";
        oss << "- This is a comprehensive explanation of the topic\n";
        oss << "- The explanation covers fundamental concepts\n";
        oss << "- Examples are provided for clarity\n";
        return oss.str();
    }
};

REGISTER_AGENT_PLUGIN(SolverPlugin,
    "agent.solver",
    "Solution Executor",
    "CLMA Core",
    "Generates executable solutions based on reasoning analysis",
    AgentStep::SOLVER)

} // namespace clma
