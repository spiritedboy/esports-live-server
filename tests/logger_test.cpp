#include "common/logger.h"

#include <iostream>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

int main() {
    std::ostringstream output;
    esports::Logger logger(output);
    logger.log(esports::LogLevel::Debug, "不应出现");
    if (!output.str().empty()) return 1;
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&logger, t]() {
            for (int i = 0; i < 100; ++i) {
                logger.log(esports::LogLevel::Info,
                           std::to_string(t) + ":" + std::to_string(i));
            }
        });
    }
    for (auto& worker : workers) worker.join();

    std::set<std::string> expected;
    for (int t = 0; t < 4; ++t)
        for (int i = 0; i < 100; ++i)
            expected.insert(std::to_string(t) + ":" + std::to_string(i));
    std::istringstream input(output.str());
    std::string line;
    int count = 0;
    while (std::getline(input, line)) {
        const std::string marker = " [INFO] ";
        const auto position = line.find(marker);
        if (position != 20 || expected.erase(line.substr(position + marker.size())) != 1)
            return 2;
        ++count;
    }
    if (count != 400 || !expected.empty()) return 3;

    std::ostringstream escaped;
    esports::Logger errors(escaped, esports::LogLevel::Warning);
    errors.log(esports::LogLevel::Info, "不应出现");
    errors.log(esports::LogLevel::Warning, "a\nb\rc");
    errors.log(esports::LogLevel::Error, "错误");
    std::istringstream escaped_input(escaped.str());
    if (!std::getline(escaped_input, line) ||
        line.substr(20) != " [WARN] a\\nb\\rc") return 4;
    if (!std::getline(escaped_input, line) ||
        line.substr(20) != " [ERROR] 错误") return 5;
    if (std::getline(escaped_input, line)) return 6;
    std::cout << "日志级别过滤、换行转义及 4 线程 400 条日志完整性测试通过\n";
    return 0;
}
