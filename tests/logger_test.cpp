/*
 * 初学者导读
 * 被测对象是 Logger；把输出写到内存字符串，测试不会依赖终端显示效果。
 * 四个线程各写 100 条带唯一编号的记录，join 后再读取字符串，避免测试自身产生数据竞争。
 * 用集合逐条删去期望编号，同时检查行数，能发现丢失、重复和两条日志拼接。
 * 后半段验证等级过滤以及换行转义；非零返回值对应失败检查的位置。
 */
// 并发写日志后检查记录完整性、等级过滤及换行转义，防止多线程消息交叉。
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
    // 并发压力部分：每个线程用自己的 t 值生成唯一编号，共用 logger 验证行级互斥。
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

    // 不要求多线程日志的先后顺序，只要求每个编号恰好出现一次；因此使用集合而非固定序列。
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

    // 另建日志器隔离前面的 400 条记录，验证过滤和转义不会增加额外物理行。
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
