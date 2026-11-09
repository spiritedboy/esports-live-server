/*
 * 初学者导读
 * 用 64×48 YUV420P 帧触发 VideoConverter 的共享引用路径，而不是缩放路径。
 * 先写一个已知像素 77，输出 make_writable 后改成 99，源仍必须保持 77。
 * 再从源创建 surviving，释放源外壳后 surviving 仍应读到 77：缓冲由引用计数保活。
 * 这同时验证“独立帧头”“共享缓冲”“写时复制”，不只是检查指针是否非空。
 */
// 检查共享像素在源释放后仍可读，并验证make_writable后的修改不会影响另一引用。
#include "codec/video_converter.h"
#include <iostream>
int main() {
    try {
        esports::VideoConverter converter;
        esports::Frame source = esports::make_frame();
        source->format = AV_PIX_FMT_YUV420P; source->width = 64; source->height = 48;
        esports::ff_check(av_frame_get_buffer(source.get(), 32), "分配源");
        source->data[0][0] = 77;
        source->pict_type = AV_PICTURE_TYPE_I;
        // 相同尺寸触发引用路径；帧类型必须清空，否则输入的 I 帧标记会干扰节目重新编码决策。
        esports::Frame shared = converter.convert(source.get(), 64, 48);
        if (shared->pict_type != AV_PICTURE_TYPE_NONE) throw std::runtime_error("泄漏输入帧类型");
        // 写时复制不能污染仍被输入帧持有的像素。
        esports::ff_check(av_frame_make_writable(shared.get()), "分离可写帧");
        shared->data[0][0] = 99;
        if (source->data[0][0] != 77) throw std::runtime_error("写时复制污染源");
        esports::Frame surviving = converter.convert(source.get(), 64, 48);
        // 主动销毁原拥有者，测试剩余引用是否独立保活像素，而非碰巧源对象尚未析构。
        source.reset();
        if (surviving->data[0][0] != 77 || shared->data[0][0] != 99)
            throw std::runtime_error("源释放后图像引用失效");
        std::cout << "源帧释放、独立帧头、写时复制所有权通过\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
