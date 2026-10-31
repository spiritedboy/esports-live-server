/*
 * 启动配置适配层：严格解析JSON，再转换成既有CLI选项交给live_main统一校验。
 * 不在这里打开媒体、创建线程或维护第二套参数默认值。
 */
#include "config/live_config.h"
#include "picojson.h"
#include <fstream>
#include <sys/stat.h>
#include <regex>
#include <set>
#include <cmath>
#include <iomanip>
#include <sstream>
extern "C" {
#include <libavutil/avstring.h>
}
namespace esports {
namespace {
void valid_text(const std::string& text) {
    const uint8_t* pos = reinterpret_cast<const uint8_t*>(text.data());
    const uint8_t* end = pos + text.size();
    while (pos < end) {
        int32_t code;
        if (av_utf8_decode(&code, &pos, end, 0) < 0 || code == 0)
            throw std::runtime_error("配置必须为有效UTF-8且不能包含NUL");
    }
}
// 使用库的事件接口拒绝重复键和嵌套对象；数组只允许标量，限制解析深度。
class ConfigContext : public picojson::default_parse_context {
public:
    ConfigContext(picojson::value* out, unsigned depth = 0) : default_parse_context(out), depth_(depth) {}
    bool parse_object_start() { return depth_ == 0 && default_parse_context::parse_object_start(); }
    bool parse_array_start() { return depth_ == 1 && default_parse_context::parse_array_start(); }
    template<class Iter> bool parse_object_item(picojson::input<Iter>& in, const std::string& key) {
        valid_text(key);
        auto& object = out_->get<picojson::object>();
        if (object.count(key)) throw std::runtime_error("配置包含重复字段：" + key);
        ConfigContext child(&object[key], depth_ + 1);
        return picojson::_parse(child, in);
    }
    template<class Iter> bool parse_array_item(picojson::input<Iter>& in, size_t index) {
        if (index >= 4) throw std::runtime_error("配置数组最多4项");
        auto& array = out_->get<picojson::array>();
        array.emplace_back();
        ConfigContext child(&array.back(), depth_ + 1);
        return picojson::_parse(child, in);
    }
    bool set_number(double number) {
        if (!std::isfinite(number)) throw std::runtime_error("配置数字必须有限");
        return default_parse_context::set_number(number);
    }
private:
    unsigned depth_;
};
void check_number_grammar(const std::string& text) {
    // PicoJSON的strtod接受部分非JSON数字；先限定词法，字符串内的数字不参与检查。
    const std::regex number("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?");
    bool string = false, escaped = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') string = false;
        } else if (c == '"') string = true;
        else if (c == '-' || (c >= '0' && c <= '9')) {
            const size_t start = i;
            while (i < text.size() && std::string("0123456789.eE+-").find(text[i]) != std::string::npos) ++i;
            if (i-start > 128 || !std::regex_match(text.substr(start, i-start), number))
                throw std::runtime_error("配置数字不符合JSON语法");
            --i;
        }
    }
}
// PicoJSON以double存数字；限制到可精确表示的整数范围，防止类型转换悄悄损失精度。
std::string scalar(const picojson::value& value, const std::string& key, bool number, bool integral = true) {
    if (!number) {
        if (!value.is<std::string>()) throw std::runtime_error(key + "必须为字符串");
        const auto& text = value.get<std::string>(); valid_text(text); return text;
    }
    if (!value.is<double>()) throw std::runtime_error(key + "必须为数字");
    const double n = value.get<double>();
    if (!std::isfinite(n) || std::fabs(n) > 9007199254740991.0 || (integral && std::floor(n) != n))
        throw std::runtime_error(key + "必须为可精确表示的有效数字，整数字段不能有小数");
    std::ostringstream out; out.imbue(std::locale::classic());
    if (integral) out << std::fixed << std::setprecision(0) << (n == 0 ? 0 : n);
    else out << std::setprecision(17) << n;
    return out.str();
}
std::string option(std::string name) { for (char& c : name) if (c == '_') c = '-'; return "--" + name; }
}
// 只读取普通文件且限制为1MiB；禁止重复键/未知键，防止配置拼错却悄悄回退默认值。
std::vector<std::string> load_live_config(const std::string& path) {
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        throw std::runtime_error("配置路径必须是可读取的普通文件：" + path);
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("无法读取配置文件：" + path);
    std::string text; char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
        text.append(buffer, static_cast<size_t>(file.gcount()));
        if (text.size() > 1024*1024) throw std::runtime_error("配置文件超过1MiB限制");
    }
    if (file.bad()) throw std::runtime_error("配置文件读取失败");
    valid_text(text); check_number_grammar(text);
    picojson::value root; ConfigContext context(&root);
    std::string error;
    auto end = picojson::_parse(context, text.begin(), text.end(), &error);
    while (end != text.end() && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) ++end;
    // 不输出库错误中的原始片段，避免无意泄露配置中的推流凭据。
    if (!error.empty() || end != text.end() || !root.is<picojson::object>())
        throw std::runtime_error("配置JSON语法错误：需要一个对象，不允许尾随内容或嵌套对象");
    const std::set<std::string> numbers = {"width","height","fps","duration_ms","stale_ms","timeout_ms",
        "output_timeout_ms","reconnect_attempts","retry_backoff_ms","output_latency_ms","packet_queue_count",
        "packet_queue_bytes","video_queue_frames","audio_queue_blocks","video_bitrate","gop_ms","vbv_ms",
        "segment_ms","record_queue_bytes","switch_at_ms","change_at_ms"};
    const std::set<std::string> strings = {"output","layout","switch_layout","stats_file","metrics_file",
        "record_prefix","record_format"};
    std::vector<std::string> args;
    const auto add = [&](const std::string& key, const std::string& value) { args.push_back(key); args.push_back(value); };
    for (const auto& entry : root.get<picojson::object>()) {
        const auto& key = entry.first; const auto& value = entry.second;
        if (numbers.count(key) || strings.count(key)) add(option(key), scalar(value, key, numbers.count(key)));
        else if (key == "realtime") {
            if (!value.is<bool>()) throw std::runtime_error("realtime必须为布尔值");
            args.push_back(value.get<bool>() ? "--realtime" : "--no-realtime");
        } else if (key == "inputs" || key == "audio_inputs" || key == "gains" || key == "muted" ||
                   key == "audio_offsets_ms" || key == "video_offsets_ms" || key == "change_gain") {
            if (!value.is<picojson::array>()) throw std::runtime_error(key + "必须为数组");
            const auto& items = value.get<picojson::array>();
            if (key == "change_gain") {
                if (items.size() != 2) throw std::runtime_error("change_gain必须为[索引,增益]");
                add("--change-gain", scalar(items[0], key, true) + ":" + scalar(items[1], key, true, false));
                continue;
            }
            if (key == "inputs" && items.empty()) throw std::runtime_error("inputs不能为空");
            for (size_t i = 0; i < items.size(); ++i) {
                if (key == "inputs" || key == "audio_inputs")
                    add(key == "inputs" ? "--input" : "--audio-input", scalar(items[i], key, false));
                else if (key == "muted") {
                    if (!items[i].is<bool>()) throw std::runtime_error("muted元素必须为布尔值");
                    add(items[i].get<bool>() ? "--mute" : "--unmute", std::to_string(i));
                } else {
                    const std::string name = key == "gains" ? "--gain" :
                        key == "audio_offsets_ms" ? "--audio-offset-ms" : "--video-offset-ms";
                    add(name, std::to_string(i) + ":" + scalar(items[i], key, true, key != "gains"));
                }
            }
        } else throw std::runtime_error("未知配置字段：" + key);
    }
    return args;
}
// 导出已经合并并校验的有效配置；不隐去URL原值，因此输出中可能包含推流凭据。
std::string serialize_live_config(const LiveOptions& o) {
    picojson::object result;
    const auto num = [&](const std::string& key, double n) { result[key] = picojson::value(n); };
    const auto str = [&](const std::string& key, const std::string& s) { result[key] = picojson::value(s); };
    const auto list = [&](const std::string& key, const std::vector<std::string>& values) {
        picojson::array items; for (const auto& v : values) items.emplace_back(v); result[key] = picojson::value(items);
    };
    list("inputs", o.inputs); list("audio_inputs", o.audio_inputs);
    str("output",o.output); str("layout",o.layout); str("stats_file",o.stats_file); str("metrics_file",o.metrics_file);
    str("record_prefix",o.recording.prefix); str("record_format",o.recording.format == "matroska" ? "mkv" : o.recording.format);
    result["realtime"] = picojson::value(o.realtime);
    num("width",o.width); num("height",o.height); num("fps",o.fps); num("duration_ms",o.duration_ms);
    num("stale_ms",o.stale_ms); num("timeout_ms",o.timeout_ms);
    num("output_timeout_ms",o.output_options.timeout_ms); num("reconnect_attempts",o.output_options.reconnect_attempts);
    num("retry_backoff_ms",o.output_options.backoff_ms); num("output_latency_ms",o.output_options.queue.latency_ms);
    num("packet_queue_count",o.output_options.queue.packets); num("packet_queue_bytes",o.output_options.queue.bytes);
    num("video_queue_frames",o.video_queue_frames); num("audio_queue_blocks",o.audio_queue_blocks);
    num("video_bitrate",o.video_bitrate); num("gop_ms",o.gop_ms); num("vbv_ms",o.vbv_ms);
    num("segment_ms",o.recording.segment_ms); num("record_queue_bytes",o.recording.queue_bytes);
    picojson::array gains, muted, ao, vo;
    for (double v : o.gains) gains.emplace_back(v);
    for (bool v : o.muted) muted.emplace_back(v);
    for (int v : o.audio_offsets) ao.emplace_back(static_cast<double>(v));
    for (int v : o.video_offsets) vo.emplace_back(static_cast<double>(v));
    result["gains"] = picojson::value(gains); result["muted"] = picojson::value(muted);
    result["audio_offsets_ms"] = picojson::value(ao); result["video_offsets_ms"] = picojson::value(vo);
    if (o.switch_at_ms >= 0) { num("switch_at_ms",o.switch_at_ms); str("switch_layout",o.switch_layout); }
    if (o.change_at_ms >= 0) {
        num("change_at_ms",o.change_at_ms);
        picojson::array change; change.emplace_back(static_cast<double>(o.change_index)); change.emplace_back(o.change_gain);
        result["change_gain"] = picojson::value(change);
    }
    return picojson::value(result).serialize(true);
}
}
