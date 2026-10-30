#include "codec/media_converter.h"
#include <iostream>
int main() {
    try {
        esports::VideoConverter converter;
        esports::Frame source = esports::make_frame();
        source->format = AV_PIX_FMT_YUV420P; source->width = 64; source->height = 48;
        esports::ff_check(av_frame_get_buffer(source.get(), 32), "分配源");
        source->data[0][0] = 77;
        source->pict_type = AV_PICTURE_TYPE_I;
        esports::Frame shared = converter.convert(source.get(), 64, 48);
        if (shared->pict_type != AV_PICTURE_TYPE_NONE) throw std::runtime_error("泄漏输入帧类型");
        // 写时复制不能污染仍被输入帧持有的像素。
        esports::ff_check(av_frame_make_writable(shared.get()), "分离可写帧");
        shared->data[0][0] = 99;
        if (source->data[0][0] != 77) throw std::runtime_error("写时复制污染源");
        esports::Frame surviving = converter.convert(source.get(), 64, 48);
        source.reset();
        if (surviving->data[0][0] != 77 || shared->data[0][0] != 99)
            throw std::runtime_error("源释放后图像引用失效");
        std::cout << "源帧释放、独立帧头、写时复制所有权通过\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
