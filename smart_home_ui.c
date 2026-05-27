/*
 * Smart6818 温湿度智能控制系统
 *
 * 功能说明：
 * 1. framebuffer 直接绘制 UI，分辨率 800x480，32bpp；
 * 2. 触摸屏设备使用 /dev/input/event0；
 * 3. 按键设备使用 /dev/input/event3；
 * 4. 前四个开关放在屏幕下方一排，严格对称；
 * 5. 中间按钮为手动 / 自动模式切换；
 * 6. 风扇使用 PWM2 控制，实际板子上接蜂鸣器，用 PWM 模拟风扇；
 * 7. 空调、窗帘、卧室灯使用 LED 设备模拟继电器；
 * 8. 温湿度没有真实 DHT11，使用软件模拟；
 * 9. 自动模式下根据温度阈值自动控制风扇、空调、窗帘；
 * 10. 温度越高，PWM2 频率越高，模拟风扇越快；
 * 11. K3/K4 控制温度加减，K6/K2 控制湿度加减；
 * 12. 右上角显示姓名（冯威成）和学号（2023020518）。
 *
 * 编译：
 *   gcc smart_home_ui.c -o smart_home_ui
 *
 * 运行：
 *   ./smart_home_ui
 *
 * 如果需要交叉编译：
 *   aarch64-linux-gnu-gcc smart_home_ui.c -o smart_home_ui
 *
 * 运行前请保证有 root 权限，因为程序需要访问：
 *   /dev/fb0
 *   /dev/input/event0
 *   /dev/input/event3
 *   /sys/class/leds
 *   /sys/class/pwm
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <linux/fb.h>
#include <linux/input.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>

/* ============================================================
 * 一、基础硬件配置
 * ============================================================ */

#define SCREEN_W 800
#define SCREEN_H 480

#define FB_DEV      "/dev/fb0"
#define TOUCH_DEV   "/dev/input/event0"
#define KEY_DEV     "/dev/input/event3"

/*
 * LED 设备节点。
 *
 * 分配如下：
 *   d7  -> 空调，模拟继电器
 *   d8  -> 窗帘，模拟继电器
 *   d10 -> 卧室灯
 *   d9  -> 自动模式指示灯
 */
#define LED_AIR_NAME       "gec6818:d7"
#define LED_CURTAIN_NAME   "gec6818:d8"
#define LED_AUTO_NAME      "gec6818:d9"
#define LED_LIGHT_NAME     "gec6818:d10"

/*
 * PWM2。
 */
#define PWM_CHIP_PATH "/sys/class/pwm/pwmchip0"
#define PWM_ID        2
#define PWM_PATH      "/sys/class/pwm/pwmchip0/pwm2"

/*
 * K3：温度 +0.5，code=115
 * K4：温度 -0.5，code=114
 * K6：湿度 +0.5，code=59
 * K2：湿度 -0.5，code=28
 */
#define KEY_K3_TEMP_UP       115
#define KEY_K4_TEMP_DOWN     114
#define KEY_K6_HUMI_UP       59
#define KEY_K2_HUMI_DOWN     28


/* ============================================================
 * 二、UI 布局配置
 * ============================================================ */

/*
 * 坐标说明：
 * framebuffer 显示坐标为左上角 (0,0)。
 * 触摸原始坐标左下角是 (0,0)，触摸 y 坐标必须翻转：
 *   screen_y = 479 - raw_y
 */

/*
 * 前四个开关在下面一排，严格对称。
 * 每个按钮宽 160，高 80。
 */
#define BTN_Y1 375
#define BTN_Y2 455

#define FAN_X1      20
#define FAN_X2      180

#define AIR_X1      220
#define AIR_X2      380

#define CURTAIN_X1  420
#define CURTAIN_X2  580

#define LIGHT_X1    620
#define LIGHT_X2    780

/*
 * 中间模式切换按钮，中心点为屏幕中心 (400,240)。
 */
#define MODE_X1 310
#define MODE_Y1 190
#define MODE_X2 490
#define MODE_Y2 290

/*
 * 温湿度调节触摸按钮。
 *
 * 中心模式按钮 (x=310~490, y=190~290) 左右各 2 个：
 *   左侧：TEMP+（升温）、TEMP-（降温）
 *   右侧：HUMI+（加湿）、HUMI-（除湿）
 *
 * 触摸替代实体按键 K3/K4/K6/K2。
 */
#define TEMP_CTRL_Y1 200
#define TEMP_CTRL_Y2 279

#define TEMPUP_X1   7
#define TEMPUP_X2   146

#define TEMPDOWN_X1 162
#define TEMPDOWN_X2 301

#define HUMIUP_X1   498
#define HUMIUP_X2   637

#define HUMIDOWN_X1 653
#define HUMIDOWN_X2 792

/*
 * 退出区域，左上角小区域。
 */
#define EXIT_X1 0
#define EXIT_Y1 0
#define EXIT_X2 90
#define EXIT_Y2 55

/*
 * 状态面板（左侧）和姓名学号面板（右侧）。
 */
#define PANEL_X1      20
#define PANEL_Y1      80
#define PANEL_X2      495
#define PANEL_Y2      178

#define NAMEID_X1     515
#define NAMEID_Y1     72
#define NAMEID_X2     780
#define NAMEID_Y2     180

/* ============================================================
 * 三、温湿度与自动控制阈值
 * ============================================================ */

#define INIT_TEMP 33.0f
#define INIT_HUMI 60.0f

#define TEMP_MIN 20.0f
#define TEMP_MAX 40.0f
#define HUMI_MIN 30.0f
#define HUMI_MAX 90.0f

#define AIR_ON_TEMP       30.0f
#define AIR_OFF_TEMP      28.0f

#define CURTAIN_ON_TEMP   31.0f
#define CURTAIN_OFF_TEMP  29.0f

/* ============================================================
 * 四、全局状态结构
 * ============================================================ */

typedef struct {
    float temp;
    float humi;

    int auto_mode;      /* 0=手动模式，1=自动模式 */

    int fan_on;         /* 风扇状态 */
    int air_on;         /* 空调状态 */
    int curtain_on;     /* 窗帘执行器状态 */
    int light_on;       /* 卧室灯状态 */

    int fan_level;      /* 风扇档位 0~4 */
    int fan_freq;       /* 当前风扇 PWM 频率 */
} SYS_STATE;

static SYS_STATE g_state;

/* ============================================================
 * 五、framebuffer 相关全局变量
 * ============================================================ */

static int fb_fd = -1;
static unsigned char *fb_mem = NULL;
static long fb_size = 0;

static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;

/* ============================================================
 * 六、输入设备文件描述符
 * ============================================================ */

static int touch_fd = -1;
static int key_fd = -1;

/* ============================================================
 * 七、Dirty Flag 机制
 *
 * 避免整屏重绘，用脏标记只刷新变化区域。
 * ============================================================ */

static volatile int need_redraw_status   = 1;
static volatile int need_redraw_mode     = 1;
static volatile int need_redraw_fan      = 1;
static volatile int need_redraw_air      = 1;
static volatile int need_redraw_curtain  = 1;
static volatile int need_redraw_light    = 1;
static volatile int need_redraw_name_id  = 1;

/* ============================================================
 * 八、基础文件操作函数
 * ============================================================ */

static int file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static int write_str_file(const char *path, const char *str)
{
    int fd;
    ssize_t ret;

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        printf("open failed: %s, errno=%d\n", path, errno);
        return -1;
    }

    ret = write(fd, str, strlen(str));
    close(fd);

    if (ret < 0) {
        printf("write failed: %s, errno=%d\n", path, errno);
        return -1;
    }

    return 0;
}

static int write_int_file(const char *path, int value)
{
    char buf[64];

    snprintf(buf, sizeof(buf), "%d", value);
    return write_str_file(path, buf);
}

static float clamp_float(float v, float min, float max)
{
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

/* ============================================================
 * 九、LED 驱动层
 * ============================================================ */

static void led_init_one(const char *name)
{
    char path[256];

    snprintf(path, sizeof(path), "/sys/class/leds/%s/trigger", name);
    write_str_file(path, "none");

    snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", name);
    write_str_file(path, "0");
}

static void led_set(const char *name, int on)
{
    char path[256];

    snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", name);

    if (on) {
        write_str_file(path, "1");
    } else {
        write_str_file(path, "0");
    }
}

static void led_init_all(void)
{
    led_init_one(LED_AIR_NAME);
    led_init_one(LED_CURTAIN_NAME);
    led_init_one(LED_LIGHT_NAME);
    led_init_one(LED_AUTO_NAME);
}

static void led_all_off(void)
{
    led_set(LED_AIR_NAME, 0);
    led_set(LED_CURTAIN_NAME, 0);
    led_set(LED_LIGHT_NAME, 0);
    led_set(LED_AUTO_NAME, 0);
}

/* ============================================================
 * 十、PWM2 风扇驱动层
 * ============================================================ */

static int pwm_export(void)
{
    char pwm_dir[256];

    snprintf(pwm_dir, sizeof(pwm_dir), "%s/pwm%d", PWM_CHIP_PATH, PWM_ID);

    if (file_exists(pwm_dir)) {
        return 0;
    }

    printf("export PWM%d\n", PWM_ID);

    if (write_int_file(PWM_CHIP_PATH "/export", PWM_ID) < 0) {
        printf("PWM export failed\n");
        return -1;
    }

    usleep(200000);
    return 0;
}

static void pwm_stop(void)
{
    write_str_file(PWM_PATH "/enable", "0");
}

static int pwm_set_freq(int freq_hz)
{
    int period_ns;
    int duty_ns;

    if (freq_hz <= 0) {
        pwm_stop();
        return 0;
    }

    /*
     * period_ns = 1秒 / 频率
     * 1秒 = 1000000000 ns
     */
    period_ns = 1000000000 / freq_hz;
    duty_ns = period_ns / 2;

    /*
     * 修改 period 和 duty_cycle 前先关闭 PWM。
     */
    pwm_stop();

    if (write_int_file(PWM_PATH "/period", period_ns) < 0) {
        return -1;
    }

    if (write_int_file(PWM_PATH "/duty_cycle", duty_ns) < 0) {
        return -1;
    }

    if (write_str_file(PWM_PATH "/enable", "1") < 0) {
        return -1;
    }

    return 0;
}

static int pwm_init(void)
{
    if (pwm_export() < 0) {
        return -1;
    }

    pwm_stop();

    /* 默认配置为 2kHz，50% 占空比。 */
    write_str_file(PWM_PATH "/period", "500000");
    write_str_file(PWM_PATH "/duty_cycle", "250000");

    return 0;
}

static void fan_set(int on, int freq)
{
    if (!on) {
        pwm_stop();
        return;
    }

    if (freq <= 0) {
        freq = 2000;
    }

    pwm_set_freq(freq);
}

/* ============================================================
 * 十一、framebuffer 绘图层
 * ============================================================ */

static int init_fb(void)
{
    fb_fd = open(FB_DEV, O_RDWR);
    if (fb_fd < 0) {
        perror("open framebuffer failed");
        return -1;
    }

    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("FBIOGET_FSCREENINFO failed");
        close(fb_fd);
        return -1;
    }

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("FBIOGET_VSCREENINFO failed");
        close(fb_fd);
        return -1;
    }

    printf("Framebuffer: %ux%u, %u bpp, line_length=%u\n",
           vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, finfo.line_length);

    if (vinfo.bits_per_pixel != 32) {
        printf("当前程序只支持 32bpp framebuffer。\n");
        return -1;
    }

    fb_size = finfo.smem_len;

    fb_mem = (unsigned char *)mmap(NULL,
                                   fb_size,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED,
                                   fb_fd,
                                   0);

    if (fb_mem == MAP_FAILED) {
        perror("mmap framebuffer failed");
        fb_mem = NULL;
        close(fb_fd);
        return -1;
    }

    return 0;
}

static void put_pixel_32(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    long location;

    if (x < 0 || x >= (int)vinfo.xres || y < 0 || y >= (int)vinfo.yres) {
        return;
    }

    location = y * finfo.line_length + x * 4;

    fb_mem[location + 0] = b;
    fb_mem[location + 1] = g;
    fb_mem[location + 2] = r;
    fb_mem[location + 3] = 0x00;
}

/*
 * 封装函数：接受 0xAARRGGBB 格式颜色，转换为 r,g,b 后调用 put_pixel_32。
 * 用于适配 weicheng-ALL.txt 中汉字/数字点阵绘制函数的颜色格式。
 */
static void draw_pixel_color(int x, int y, int color_hex)
{
    uint8_t r, g, b;

    r = (color_hex >> 16) & 0xFF;
    g = (color_hex >> 8) & 0xFF;
    b = color_hex & 0xFF;

    put_pixel_32(x, y, r, g, b);
}

static void draw_rect(int x1, int y1, int x2, int y2,
                      uint8_t r, uint8_t g, uint8_t b)
{
    int x, y;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= SCREEN_W) x2 = SCREEN_W - 1;
    if (y2 >= SCREEN_H) y2 = SCREEN_H - 1;

    for (y = y1; y <= y2; y++) {
        for (x = x1; x <= x2; x++) {
            put_pixel_32(x, y, r, g, b);
        }
    }
}

static void draw_frame(int x1, int y1, int x2, int y2,
                       int thickness,
                       uint8_t r, uint8_t g, uint8_t b)
{
    int i;

    for (i = 0; i < thickness; i++) {
        draw_rect(x1 + i, y1 + i, x2 - i, y1 + i, r, g, b);
        draw_rect(x1 + i, y2 - i, x2 - i, y2 - i, r, g, b);
        draw_rect(x1 + i, y1 + i, x1 + i, y2 - i, r, g, b);
        draw_rect(x2 - i, y1 + i, x2 - i, y2 - i, r, g, b);
    }
}

static int in_rect(int x, int y, int x1, int y1, int x2, int y2)
{
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
}

/* ============================================================
 * 十二、简单 ASCII 字库（5x7 点阵）
 * ============================================================ */

static const uint8_t *font_get(char c)
{
    static const uint8_t space[7] = {0,0,0,0,0,0,0};
    static const uint8_t unknown[7] = {31,17,2,4,4,0,4};

    static const uint8_t n0[7] = {14,17,19,21,25,17,14};
    static const uint8_t n1[7] = {4,12,4,4,4,4,14};
    static const uint8_t n2[7] = {14,17,1,2,4,8,31};
    static const uint8_t n3[7] = {30,1,1,14,1,1,30};
    static const uint8_t n4[7] = {2,6,10,18,31,2,2};
    static const uint8_t n5[7] = {31,16,30,1,1,17,14};
    static const uint8_t n6[7] = {6,8,16,30,17,17,14};
    static const uint8_t n7[7] = {31,1,2,4,8,8,8};
    static const uint8_t n8[7] = {14,17,17,14,17,17,14};
    static const uint8_t n9[7] = {14,17,17,15,1,2,12};

    static const uint8_t A[7] = {14,17,17,31,17,17,17};
    static const uint8_t B[7] = {30,17,17,30,17,17,30};
    static const uint8_t C[7] = {14,17,16,16,16,17,14};
    static const uint8_t D[7] = {30,17,17,17,17,17,30};
    static const uint8_t E[7] = {31,16,16,30,16,16,31};
    static const uint8_t F[7] = {31,16,16,30,16,16,16};
    static const uint8_t G[7] = {14,17,16,23,17,17,15};
    static const uint8_t H[7] = {17,17,17,31,17,17,17};
    static const uint8_t I[7] = {14,4,4,4,4,4,14};
    static const uint8_t J[7] = {1,1,1,1,17,17,14};
    static const uint8_t K[7] = {17,18,20,24,20,18,17};
    static const uint8_t L[7] = {16,16,16,16,16,16,31};
    static const uint8_t M[7] = {17,27,21,21,17,17,17};
    static const uint8_t N[7] = {17,25,21,19,17,17,17};
    static const uint8_t O[7] = {14,17,17,17,17,17,14};
    static const uint8_t P[7] = {30,17,17,30,16,16,16};
    static const uint8_t Q[7] = {14,17,17,17,21,18,13};
    static const uint8_t R[7] = {30,17,17,30,20,18,17};
    static const uint8_t S[7] = {15,16,16,14,1,1,30};
    static const uint8_t T[7] = {31,4,4,4,4,4,4};
    static const uint8_t U[7] = {17,17,17,17,17,17,14};
    static const uint8_t V[7] = {17,17,17,17,17,10,4};
    static const uint8_t W[7] = {17,17,17,21,21,21,10};
    static const uint8_t X[7] = {17,17,10,4,10,17,17};
    static const uint8_t Y[7] = {17,17,10,4,4,4,4};
    static const uint8_t Z[7] = {31,1,2,4,8,16,31};

    static const uint8_t colon[7] = {0,4,4,0,4,4,0};
    static const uint8_t dot[7] = {0,0,0,0,0,4,4};
    static const uint8_t percent[7] = {24,25,2,4,8,19,3};
    static const uint8_t minus[7] = {0,0,0,31,0,0,0};
    static const uint8_t plus[7]  = {0,4,4,31,4,4,0};
    static const uint8_t slash[7] = {1,1,2,4,8,16,16};

    if (c >= 'a' && c <= 'z') {
        c = c - 'a' + 'A';
    }

    switch (c) {
    case ' ': return space;
    case '0': return n0;
    case '1': return n1;
    case '2': return n2;
    case '3': return n3;
    case '4': return n4;
    case '5': return n5;
    case '6': return n6;
    case '7': return n7;
    case '8': return n8;
    case '9': return n9;

    case 'A': return A;
    case 'B': return B;
    case 'C': return C;
    case 'D': return D;
    case 'E': return E;
    case 'F': return F;
    case 'G': return G;
    case 'H': return H;
    case 'I': return I;
    case 'J': return J;
    case 'K': return K;
    case 'L': return L;
    case 'M': return M;
    case 'N': return N;
    case 'O': return O;
    case 'P': return P;
    case 'Q': return Q;
    case 'R': return R;
    case 'S': return S;
    case 'T': return T;
    case 'U': return U;
    case 'V': return V;
    case 'W': return W;
    case 'X': return X;
    case 'Y': return Y;
    case 'Z': return Z;

    case ':': return colon;
    case '.': return dot;
    case '%': return percent;
    case '+': return plus;
    case '-': return minus;
    case '/': return slash;

    default:
        return unknown;
    }
}

static void draw_char(int x, int y, char c, int scale,
                      uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t *font = font_get(c);
    int row, col;

    for (row = 0; row < 7; row++) {
        for (col = 0; col < 5; col++) {
            if (font[row] & (1 << (4 - col))) {
                draw_rect(x + col * scale,
                          y + row * scale,
                          x + col * scale + scale - 1,
                          y + row * scale + scale - 1,
                          r, g, b);
            }
        }
    }
}

static void draw_text(int x, int y, const char *s, int scale,
                      uint8_t r, uint8_t g, uint8_t b)
{
    int cursor = x;

    while (*s) {
        draw_char(cursor, y, *s, scale, r, g, b);
        cursor += 6 * scale;
        s++;
    }
}

/* ============================================================
 * 十三、汉字和数字点阵数据
 *
 * 姓名：冯威成（3 个汉字，每个 40x46 点阵）
 * 学号：2023020518（10 个数字，每个 24x46 点阵）
 *
 * 数据来源：weicheng-ALL.txt
 * 已适配：颜色格式 0xAARRGGBB → 通过 draw_pixel_color 转成 r,g,b
 * ============================================================ */

unsigned char g_Hzk_Name[] = {
/*--  文字:  冯  --*/
/*--  微软雅黑26;  此字体下对应的点阵为：宽x高=35x46   --*/
/*--  宽度不是8的倍数，现调整为：宽度x高度=40x46  --*/
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7F,0xFF,
0xFE,0x00,0x30,0x7F,0xFF,0xFE,0x00,0x78,0x7F,0xFF,0xFC,0x00,0xFC,0x00,0x00,0x3C,
0x00,0x7E,0x0E,0x00,0x3C,0x00,0x3F,0x0F,0x00,0x3C,0x00,0x1F,0x1F,0x00,0x3C,0x00,
0x0F,0x9F,0x00,0x3C,0x00,0x07,0x9E,0x00,0x3C,0x00,0x07,0x1E,0x00,0x3C,0x00,0x00,
0x1E,0x00,0x3C,0x00,0x00,0x1E,0x00,0x3C,0x00,0x00,0x1E,0x00,0x7C,0x00,0x00,0x1E,
0x00,0x7C,0x00,0x06,0x1F,0xFF,0xFF,0xC0,0x07,0x1F,0xFF,0xFF,0xC0,0x0F,0x9F,0xFF,
0xFF,0xC0,0x0F,0x80,0x00,0x03,0xC0,0x0F,0x00,0x00,0x03,0xC0,0x0F,0x00,0x00,0x03,
0xC0,0x1F,0x00,0x00,0x03,0xC0,0x1E,0x7F,0xFF,0xF3,0xC0,0x1E,0xFF,0xFF,0xFB,0xC0,
0x3E,0xFF,0xFF,0xFB,0xC0,0x3C,0xFF,0xFF,0xFF,0xC0,0x7C,0x00,0x00,0x07,0xC0,0x7C,
0x00,0x00,0x07,0xC0,0x78,0x00,0x00,0x07,0x80,0xF8,0x00,0x00,0x0F,0x80,0x78,0x00,
0x1F,0xFF,0x80,0x10,0x00,0x1F,0xFF,0x00,0x00,0x00,0x1F,0xFE,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,

/*--  文字:  威  --*/
/*--  微软雅黑26;  此字体下对应的点阵为：宽x高=35x46   --*/
/*--  宽度不是8的倍数，现调整为：宽度x高度=40x46  --*/
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x9C,0x00,0x00,0x00,0x07,
0xBE,0x00,0x00,0x00,0x07,0x9F,0x00,0x00,0x00,0x07,0x8F,0x80,0x00,0x00,0x07,0x87,
0x00,0x1F,0xFF,0xFF,0xFF,0xE0,0x1F,0xFF,0xFF,0xFF,0xE0,0x1F,0xFF,0xFF,0xFF,0xE0,
0x1E,0x00,0x07,0x80,0x00,0x1E,0x00,0x07,0x80,0x00,0x1F,0xFF,0xFF,0x80,0x00,0x1F,
0xFF,0xFF,0x87,0x80,0x1F,0xFF,0xFF,0x87,0x80,0x1E,0x00,0x07,0xC7,0x80,0x1E,0x0F,
0x03,0xCF,0x80,0x1E,0x1F,0x03,0xCF,0x00,0x1F,0xFF,0xF3,0xCF,0x00,0x1F,0xFF,0xFB,
0xDE,0x00,0x1F,0xFF,0xFB,0xDE,0x00,0x1F,0xFF,0xFB,0xFE,0x00,0x1E,0x78,0xFB,0xFC,
0x00,0x1E,0x78,0xF1,0xFC,0x00,0x1E,0xF1,0xF1,0xF8,0x00,0x1E,0xFD,0xE1,0xF0,0x00,
0x1E,0x7F,0xC1,0xF1,0xE0,0x3C,0x1F,0xC1,0xF1,0xE0,0x3C,0x0F,0xF3,0xF1,0xE0,0x3C,
0x3F,0xF7,0xF9,0xE0,0x7C,0x7E,0xFF,0xFB,0xE0,0x79,0xFC,0x7F,0x7F,0xC0,0xF9,0xF8,
0x3E,0x3F,0xC0,0xF1,0xE0,0x7C,0x3F,0xC0,0x70,0x00,0x18,0x1F,0x80,0x20,0x00,0x00,
0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,

/*--  文字:  成  --*/
/*--  微软雅黑26;  此字体下对应的点阵为：宽x高=35x46   --*/
/*--  宽度不是8的倍数，现调整为：宽度x高度=40x46  --*/
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1E,0x70,0x00,0x00,0x00,0x1E,
0xFC,0x00,0x00,0x00,0x1E,0x7E,0x00,0x00,0x00,0x1E,0x3F,0x00,0x00,0x00,0x1E,0x0F,
0x00,0x00,0x00,0x1E,0x06,0x00,0x1F,0xFF,0xFF,0xFF,0xE0,0x1F,0xFF,0xFF,0xFF,0xE0,
0x1F,0xFF,0xFF,0xFF,0xE0,0x1F,0x00,0x1E,0x00,0x00,0x1F,0x00,0x1E,0x00,0x00,0x1F,
0x00,0x1E,0x00,0x00,0x1F,0x00,0x1E,0x07,0x00,0x1F,0xFF,0x9E,0x0F,0x80,0x1F,0xFF,
0x9F,0x0F,0x00,0x1F,0xFF,0x9F,0x1F,0x00,0x1F,0xFF,0x8F,0x1E,0x00,0x1F,0x07,0x8F,
0x3E,0x00,0x1E,0x07,0x8F,0x3C,0x00,0x1E,0x07,0x8F,0x7C,0x00,0x1E,0x07,0x8F,0x78,
0x00,0x1E,0x07,0x8F,0xF8,0x00,0x1E,0x07,0x87,0xF0,0x00,0x1E,0x07,0x87,0xE0,0x80,
0x3E,0x07,0x87,0xE1,0xE0,0x3C,0x0F,0x87,0xC1,0xE0,0x3D,0xFF,0x8F,0xE1,0xE0,0x3D,
0xFF,0x3F,0xE1,0xE0,0x7D,0xFE,0x7F,0xF1,0xE0,0x78,0xF8,0xFC,0xFB,0xE0,0xF8,0x01,
0xF8,0x7F,0xC0,0xF0,0x01,0xF0,0x7F,0xC0,0xF0,0x00,0xE0,0x1F,0x80,0x60,0x00,0x00,
0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,

};

/*
 * 学号数字点阵：2023020518（10 个数字）
 * 每个数字 24x46 点阵，每行 24 bit = 3 字节，每个数字 3*46 = 138 字节
 * 偏移：&g_Hzk_Num[i * 138]
 */
unsigned char g_Hzk_Num[] = {
/*--  文字:  2  --*/
/*--  微软雅黑26;  此字体下对应的点阵为：宽x高=21x46   --*/
/*--  宽度不是8的倍数，现调整为：宽度x高度=24x46  --*/
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0xFC,
0x00,0x0F,0xFF,0x00,0x1F,0xFF,0x80,0x3F,0x1F,0xC0,0x3C,0x07,0xC0,0x38,0x07,0xC0,
0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x03,0xC0,0x00,0x07,0xC0,0x00,
0x0F,0xC0,0x00,0x0F,0x80,0x00,0x3F,0x00,0x00,0x7E,0x00,0x00,0xFC,0x00,0x03,0xF8,
0x00,0x07,0xF0,0x00,0x0F,0xC0,0x00,0x1F,0x80,0x00,0x1F,0x00,0x00,0x3E,0x00,0x00,
0x3C,0x00,0x00,0x7C,0x00,0x00,0x7F,0xFF,0xE0,0x7F,0xFF,0xE0,0x7F,0xFF,0xE0,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

/*--  文字:  0  --*/
/*--  微软雅黑26;  此字体下对应的点阵为：宽x高=21x46   --*/
/*--  宽度不是8的倍数，现调整为：宽度x高度=24x46  --*/
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0xFE,
0x00,0x07,0xFF,0x00,0x0F,0xFF,0x80,0x1F,0x9F,0xC0,0x1F,0x07,0xC0,0x3E,0x07,0xE0,
0x3E,0x03,0xE0,0x3C,0x03,0xE0,0x7C,0x03,0xE0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,
0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,
0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xE0,0x7C,0x03,0xE0,0x7C,0x03,0xE0,0x3E,0x03,0xE0,
0x3E,0x07,0xC0,0x1F,0x0F,0xC0,0x1F,0xFF,0x80,0x0F,0xFF,0x00,0x07,0xFE,0x00,0x00,
0xF8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

/*--  文字:  2  --*/
/*--  微软雅黑26;  此字体下对应的点阵为：宽x高=21x46   --*/
/*--  宽度不是8的倍数，现调整为：宽度x高度=24x46  --*/
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0xFC,
0x00,0x0F,0xFF,0x00,0x1F,0xFF,0x80,0x3F,0x1F,0xC0,0x3C,0x07,0xC0,0x38,0x07,0xC0,
0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x03,0xC0,0x00,0x07,0xC0,0x00,
0x0F,0xC0,0x00,0x0F,0x80,0x00,0x3F,0x00,0x00,0x7E,0x00,0x00,0xFC,0x00,0x03,0xF8,
0x00,0x07,0xF0,0x00,0x0F,0xC0,0x00,0x1F,0x80,0x00,0x1F,0x00,0x00,0x3E,0x00,0x00,
0x3C,0x00,0x00,0x7C,0x00,0x00,0x7F,0xFF,0xE0,0x7F,0xFF,0xE0,0x7F,0xFF,0xE0,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

/*--  文字:  3  --*/
/*--  微软雅黑26;  此字体下对应的点阵为：宽x高=21x46   --*/
/*--  宽度不是8的倍数，现调整为：宽度x高度=24x46  --*/
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0xFC,
0x00,0x1F,0xFE,0x00,0x1F,0xFF,0x00,0x1E,0x1F,0x80,0x18,0x0F,0x80,0x00,0x07,0xC0,
0x00,0x07,0xC0,0x00,0x07,0xC0,0x00,0x07,0xC0,0x00,0x0F,0x80,0x00,0x1F,0x80,0x00,
0xFF,0x00,0x0F,0xFE,0x00,0x0F,0xFE,0x00,0x0F,0xFF,0x00,0x00,0x1F,0x80,0x00,0x0F,
0xC0,0x00,0x07,0xC0,0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x07,0xC0,
0x00,0x07,0xC0,0x38,0x0F,0xC0,0x3F,0xFF,0x80,0x3F,0xFF,0x00,0x3F,0xFE,0x00,0x07,
0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

/*--  文字:  0  --*/
/*--  微软雅黑26;  此字体下对应的点阵为：宽x高=21x46   --*/
/*--  宽度不是8的倍数，现调整为：宽度x高度=24x46  --*/
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0xFE,
0x00,0x07,0xFF,0x00,0x0F,0xFF,0x80,0x1F,0x9F,0xC0,0x1F,0x07,0xC0,0x3E,0x07,0xE0,
0x3E,0x03,0xE0,0x3C,0x03,0xE0,0x7C,0x03,0xE0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,
0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,
0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xE0,0x7C,0x03,0xE0,0x7C,0x03,0xE0,0x3E,0x03,0xE0,
0x3E,0x07,0xC0,0x1F,0x0F,0xC0,0x1F,0xFF,0x80,0x0F,0xFF,0x00,0x07,0xFE,0x00,0x00,
0xF8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

/*--  文字:  2  --*/
/*--  微软雅黑26;  此字体下对应的点阵为：宽x高=21x46   --*/
/*--  宽度不是8的倍数，现调整为：宽度x高度=24x46  --*/
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0xFC,
0x00,0x0F,0xFF,0x00,0x1F,0xFF,0x80,0x3F,0x1F,0xC0,0x3C,0x07,0xC0,0x38,0x07,0xC0,
0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x03,0xC0,0x00,0x07,0xC0,0x00,
0x0F,0xC0,0x00,0x0F,0x80,0x00,0x3F,0x00,0x00,0x7E,0x00,0x00,0xFC,0x00,0x03,0xF8,
0x00,0x07,0xF0,0x00,0x0F,0xC0,0x00,0x1F,0x80,0x00,0x1F,0x00,0x00,0x3E,0x00,0x00,
0x3C,0x00,0x00,0x7C,0x00,0x00,0x7F,0xFF,0xE0,0x7F,0xFF,0xE0,0x7F,0xFF,0xE0,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

/*--  文字:  0  --*/
/*--  微软雅黑26;  此字体下对应的点阵为：宽x高=21x46   --*/
/*--  宽度不是8的倍数，现调整为：宽度x高度=24x46  --*/
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0xFE,
0x00,0x07,0xFF,0x00,0x0F,0xFF,0x80,0x1F,0x9F,0xC0,0x1F,0x07,0xC0,0x3E,0x07,0xE0,
0x3E,0x03,0xE0,0x3C,0x03,0xE0,0x7C,0x03,0xE0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,
0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,
0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xE0,0x7C,0x03,0xE0,0x7C,0x03,0xE0,0x3E,0x03,0xE0,
0x3E,0x07,0xC0,0x1F,0x0F,0xC0,0x1F,0xFF,0x80,0x0F,0xFF,0x00,0x07,0xFE,0x00,0x00,
0xF8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

/*--  文字:  5  --*/
/*--  微软雅黑26;  此字体下对应的点阵为：宽x高=21x46   --*/
/*--  宽度不是8的倍数，现调整为：宽度x高度=24x46  --*/
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0xFF,
0x80,0x0F,0xFF,0x80,0x1F,0xFF,0x80,0x1F,0xFF,0x80,0x1F,0x00,0x00,0x1F,0x00,0x00,
0x1F,0x00,0x00,0x1F,0x00,0x00,0x1F,0x00,0x00,0x1E,0x00,0x00,0x1E,0xC0,0x00,0x1F,
0xFC,0x00,0x1F,0xFF,0x00,0x1F,0xFF,0x80,0x00,0x1F,0xC0,0x00,0x0F,0xC0,0x00,0x07,
0xC0,0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x03,0xE0,0x00,0x07,0xC0,
0x00,0x07,0xC0,0x38,0x1F,0xC0,0x3F,0xFF,0x80,0x3F,0xFF,0x00,0x3F,0xFE,0x00,0x07,
0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

/*--  文字:  1  --*/
/*--  微软雅黑26;  此字体下对应的点阵为：宽x高=21x46   --*/
/*--  宽度不是8的倍数，现调整为：宽度x高度=24x46  --*/
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3C,
0x00,0x00,0x7C,0x00,0x01,0xFC,0x00,0x0F,0xFC,0x00,0x1F,0xFC,0x00,0x1F,0xFC,0x00,
0x1E,0x7C,0x00,0x18,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,
0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,
0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,
0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

/*--  文字:  8  --*/
/*--  微软雅黑26;  此字体下对应的点阵为：宽x高=21x46   --*/
/*--  宽度不是8的倍数，现调整为：宽度x高度=24x46  --*/
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0xFC,
0x00,0x07,0xFF,0x00,0x0F,0xFF,0x80,0x1F,0x8F,0xC0,0x3F,0x07,0xC0,0x3E,0x03,0xC0,
0x3E,0x03,0xC0,0x3E,0x03,0xC0,0x3E,0x03,0xC0,0x1E,0x07,0xC0,0x1F,0x8F,0x80,0x0F,
0xFF,0x00,0x07,0xFE,0x00,0x0F,0xFF,0x80,0x1F,0xFF,0xC0,0x3F,0x07,0xC0,0x3E,0x03,
0xE0,0x7C,0x01,0xE0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xF0,0x7C,0x01,0xE0,
0x7E,0x03,0xE0,0x3F,0x07,0xE0,0x1F,0xFF,0xC0,0x0F,0xFF,0x80,0x07,0xFF,0x00,0x00,
0xF8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

};


/* ============================================================
 * 十四、点阵绘制函数
 *
 * 来源：weicheng-ALL.txt（已适配 800x480 + put_pixel_32）
 * 颜色格式：0xAARRGGBB → 通过 draw_pixel_color() 转换
 * ============================================================ */

/*
 * 绘制 24x46 数字点阵
 *
 * 参数：
 *   x, y  : 左上角坐标
 *   mat   : 点阵数据指针
 *   color : 0xAARRGGBB 格式颜色
 */
static void draw_num_24x46(int x, int y, unsigned char *mat, int color)
{
    int i, j, n;

    for (i = 0; i < 46; i++) {
        for (j = 0; j < 3; j++) {
            unsigned char temp = mat[i * 3 + j];
            for (n = 0; n < 8; n++) {
                if (temp & (0x80 >> n)) {
                    draw_pixel_color(x + j * 8 + n, y + i, color);
                }
            }
        }
    }
}

/*
 * 绘制 40x46 汉字点阵
 *
 * 参数：
 *   x, y  : 左上角坐标
 *   mat   : 点阵数据指针
 *   color : 0xAARRGGBB 格式颜色
 */
static void draw_hz_40x46(int x, int y, unsigned char *mat, int color)
{
    int i, j, n;

    for (i = 0; i < 46; i++) {
        for (j = 0; j < 5; j++) {
            unsigned char temp = mat[i * 5 + j];
            for (n = 0; n < 8; n++) {
                if (temp & (0x80 >> n)) {
                    draw_pixel_color(x + j * 8 + n, y + i, color);
                }
            }
        }
    }
}


/* ============================================================
 * 十五、UI 部分绘制函数
 *
 * 所有函数只操作自己的区域，互不干扰。
 * 按钮函数使用 draw_button() 完整重绘按钮区域。
 * ============================================================ */

static void draw_button(int x1, int y1, int x2, int y2,
                        const char *label,
                        int on,
                        int special_mode_button)
{
    uint8_t r, g, b;
    int text_x;
    int text_y;

    if (special_mode_button) {
        if (on) {
            r = 0; g = 170; b = 70;
        } else {
            r = 40; g = 90; b = 190;
        }
    } else {
        if (on) {
            r = 0; g = 170; b = 70;
        } else {
            r = 80; g = 80; b = 80;
        }
    }

    draw_rect(x1, y1, x2, y2, r, g, b);
    draw_frame(x1, y1, x2, y2, 3, 255, 255, 255);

    text_x = x1 + 18;
    text_y = y1 + 18;

    draw_text(text_x, text_y, label, 3, 255, 255, 255);

    if (!special_mode_button) {
        if (on) {
            draw_text(x1 + 55, y1 + 52, "ON", 2, 255, 255, 255);
        } else {
            draw_text(x1 + 50, y1 + 52, "OFF", 2, 255, 255, 255);
        }
    }
}

/*
 * 绘制温湿度调节动作按钮（蓝色外观，无 ON/OFF 状态）。
 * 用于触摸替代实体按键 K3/K4/K6/K2。
 */
static void draw_ctrl_button(int x1, int y1, int x2, int y2, const char *label)
{
    draw_rect(x1, y1, x2, y2, 50, 115, 210);
    draw_frame(x1, y1, x2, y2, 3, 255, 255, 255);
    draw_text(x1 + 18, y1 + 18, label, 3, 255, 255, 255);
}

/* ============================================================
 * 十五 - A：draw_static_ui()
 *
 * 只绘制程序启动后永不变化的内容：
 *   - 背景
 *   - 标题栏
 *   - 退出按钮
 *   - 状态面板背景 + 边框
 *   - 姓名学号面板背景 + 边框
 *   - 使用说明文字
 * ============================================================ */

static void draw_static_ui(void)
{
    /* 背景 */
    draw_rect(0, 0, SCREEN_W - 1, SCREEN_H - 1, 25, 32, 45);

    /* 顶部标题栏 */
    draw_rect(0, 0, SCREEN_W - 1, 65, 30, 75, 125);
    draw_text(110, 15, "TEMP HUMI CONTROL SYSTEM", 3, 255, 255, 255);

    /* 左上角退出区域 */
    draw_rect(EXIT_X1, EXIT_Y1, EXIT_X2, EXIT_Y2, 150, 35, 35);
    draw_frame(EXIT_X1, EXIT_Y1, EXIT_X2, EXIT_Y2, 2, 255, 255, 255);
    draw_text(12, 18, "EXIT", 2, 255, 255, 255);

    /* 左侧状态面板背景 + 边框 */
    draw_rect(PANEL_X1, PANEL_Y1, PANEL_X2, PANEL_Y2, 45, 52, 65);
    draw_frame(PANEL_X1, PANEL_Y1, PANEL_X2, PANEL_Y2, 2, 120, 140, 160);

    /* 右侧姓名学号面板背景 + 边框 */
    draw_rect(NAMEID_X1, NAMEID_Y1, NAMEID_X2, NAMEID_Y2, 45, 52, 65);
    draw_frame(NAMEID_X1, NAMEID_Y1, NAMEID_X2, NAMEID_Y2, 2, 120, 140, 160);

    /*
     * 温湿度调节按钮（模式按钮左右各 2 个）。
     * 触摸直接调节温湿度，替代实体按键 K3/K4/K6/K2。
     */
    draw_ctrl_button(TEMPUP_X1,   TEMP_CTRL_Y1, TEMPUP_X2,   TEMP_CTRL_Y2, "TEMP+");
    draw_ctrl_button(TEMPDOWN_X1, TEMP_CTRL_Y1, TEMPDOWN_X2, TEMP_CTRL_Y2, "TEMP-");
    draw_ctrl_button(HUMIUP_X1,   TEMP_CTRL_Y1, HUMIUP_X2,   TEMP_CTRL_Y2, "HUMI+");
    draw_ctrl_button(HUMIDOWN_X1, TEMP_CTRL_Y1, HUMIDOWN_X2, TEMP_CTRL_Y2, "HUMI-");

    /* 自动控制说明文字 */
    draw_text(85, 320, "K3/K4 TEMP +/-   K6/K2 HUMI +/-", 2, 180, 220, 255);
}

/* ============================================================
 * 十五 - B：draw_status_panel()
 *
 * 刷新状态面板中的所有动态文本：
 *   - 模式、温度、湿度
 *   - 风扇档位/频率、空调、窗帘、卧室灯状态
 * ============================================================ */

static void draw_status_panel(void)
{
    char buf[128];
    int inner_x1, inner_y1, inner_x2, inner_y2;

    /* 只清除面板内部区域（保留边框） */
    inner_x1 = PANEL_X1 + 3;
    inner_y1 = PANEL_Y1 + 3;
    inner_x2 = PANEL_X2 - 3;
    inner_y2 = PANEL_Y2 - 3;
    draw_rect(inner_x1, inner_y1, inner_x2, inner_y2, 45, 52, 65);

    /* 第一行：模式、温度、湿度 */
    snprintf(buf, sizeof(buf), "MODE:%s", g_state.auto_mode ? "AUTO" : "MANUAL");
    draw_text(PANEL_X1 + 8, PANEL_Y1 + 12, buf, 2, 255, 255, 255);

    snprintf(buf, sizeof(buf), "TEMP:%.1fC", g_state.temp);
    draw_text(PANEL_X1 + 150, PANEL_Y1 + 12, buf, 2, 255, 255, 255);

    snprintf(buf, sizeof(buf), "HUMI:%.1f%%", g_state.humi);
    draw_text(PANEL_X1 + 300, PANEL_Y1 + 12, buf, 2, 255, 255, 255);

    /* 第二行：风扇、空调、窗帘、卧室灯 */
    if (g_state.fan_on) {
        snprintf(buf, sizeof(buf), "FAN:L%d %dHZ", g_state.fan_level, g_state.fan_freq);
    } else {
        snprintf(buf, sizeof(buf), "FAN:OFF");
    }
    draw_text(PANEL_X1 + 8, PANEL_Y1 + 50, buf, 2, 255, 230, 80);

    snprintf(buf, sizeof(buf), "AIR:%s", g_state.air_on ? "ON" : "OFF");
    draw_text(PANEL_X1 + 150, PANEL_Y1 + 50, buf, 2, 255, 230, 80);

    snprintf(buf, sizeof(buf), "CUR:%s", g_state.curtain_on ? "ON" : "OFF");
    draw_text(PANEL_X1 + 250, PANEL_Y1 + 50, buf, 2, 255, 230, 80);

    snprintf(buf, sizeof(buf), "LIGHT:%s", g_state.light_on ? "ON" : "OFF");
    draw_text(PANEL_X1 + 350, PANEL_Y1 + 50, buf, 2, 255, 230, 80);
}

/* ============================================================
 * 十五 - C：各按钮绘制函数
 * ============================================================ */

static void draw_mode_button(void)
{
    if (g_state.auto_mode) {
        draw_button(MODE_X1, MODE_Y1, MODE_X2, MODE_Y2, "AUTO", 1, 1);
    } else {
        draw_button(MODE_X1, MODE_Y1, MODE_X2, MODE_Y2, "MANUAL", 0, 1);
    }
}

static void draw_fan_button(void)
{
    draw_button(FAN_X1, BTN_Y1, FAN_X2, BTN_Y2, "FAN", g_state.fan_on, 0);
}

static void draw_air_button(void)
{
    draw_button(AIR_X1, BTN_Y1, AIR_X2, BTN_Y2, "AIR", g_state.air_on, 0);
}

static void draw_curtain_button(void)
{
    draw_button(CURTAIN_X1, BTN_Y1, CURTAIN_X2, BTN_Y2, "CURT", g_state.curtain_on, 0);
}

static void draw_light_button(void)
{
    draw_button(LIGHT_X1, BTN_Y1, LIGHT_X2, BTN_Y2, "LAMP", g_state.light_on, 0);
}

/* ============================================================
 * 十五 - D：draw_name_id_area()
 *
 * 在右侧面板中绘制姓名和学号点阵。
 * 姓名使用 40x46 汉字点阵，学号使用 24x46 数字点阵。
 *
 * 布局（面板 x=515..780, y=72..180）：
 *   第一行：姓名 "冯威成"
 *   第二行：学号 "2023020518"
 * ============================================================ */

static void draw_name_id_area(void)
{
    int inner_x1, inner_y1, inner_x2, inner_y2;
    int panel_w;
    int name_x, name_y;
    const char *stu_id = "202302020518";

    /* 清除面板内部区域（保留边框） */
    inner_x1 = NAMEID_X1 + 3;
    inner_y1 = NAMEID_Y1 + 3;
    inner_x2 = NAMEID_X2 - 3;
    inner_y2 = NAMEID_Y2 - 3;
    draw_rect(inner_x1, inner_y1, inner_x2, inner_y2, 45, 52, 65);

    panel_w = NAMEID_X2 - NAMEID_X1;   /* 780 - 515 = 265 */

    /*
     * 姓名：3 个汉字，每个 40x46
     * 字间距 6px：40 * 3 + 6 * 2 = 132
     * 水平居中：(265 - 132) / 2 = 66.5 → 67
     */
    name_x = NAMEID_X1 + 67;
    name_y = NAMEID_Y1 + 8;

    draw_hz_40x46(name_x, name_y, &g_Hzk_Name[0], 0xFFFFFF);      /* 冯 */
    draw_hz_40x46(name_x + 46, name_y, &g_Hzk_Name[230], 0xFFFFFF);  /* 威 */
    draw_hz_40x46(name_x + 92, name_y, &g_Hzk_Name[460], 0xFFFFFF);  /* 成 */

    /*
     * 学号使用 ASCII 5x7 字体 scale=3，每个数字 18px 宽。
     * 不再使用 24x46 大点阵，避免溢出面板边界。
     * 12 位学号：12 * 18 = 216px，在 265px 面板内居中。
     */
    {
        int num_chars = (int)strlen(stu_id);
        int num_width = num_chars * 6 * 3;
        int num_x = NAMEID_X1 + (panel_w - num_width) / 2;
        int num_y = name_y + 46 + 6;

        draw_text(num_x, num_y, stu_id, 3, 255, 255, 128);
    }
}

/* ============================================================
 * 十五 - E：dirty flag 统一刷新入口
 *
 * 在主循环末尾调用，根据脏标记只刷新变化区域。
 * ============================================================ */

static void flush_dirty(void)
{
    if (need_redraw_status) {
        draw_status_panel();
        need_redraw_status = 0;
    }

    if (need_redraw_mode) {
        draw_mode_button();
        need_redraw_mode = 0;
    }

    if (need_redraw_fan) {
        draw_fan_button();
        need_redraw_fan = 0;
    }

    if (need_redraw_air) {
        draw_air_button();
        need_redraw_air = 0;
    }

    if (need_redraw_curtain) {
        draw_curtain_button();
        need_redraw_curtain = 0;
    }

    if (need_redraw_light) {
        draw_light_button();
        need_redraw_light = 0;
    }

    if (need_redraw_name_id) {
        draw_name_id_area();
        need_redraw_name_id = 0;
    }
}

/* ============================================================
 * 十六、设备状态同步到底层
 * ============================================================ */

static void apply_outputs(void)
{
    fan_set(g_state.fan_on, g_state.fan_freq);

    led_set(LED_AIR_NAME, g_state.air_on);
    led_set(LED_CURTAIN_NAME, g_state.curtain_on);
    led_set(LED_LIGHT_NAME, g_state.light_on);

    led_set(LED_AUTO_NAME, g_state.auto_mode);
}

/* ============================================================
 * 十七、模拟温湿度
 * ============================================================ */

static void sensor_sim_update_random(void)
{
    int rt;
    int rh;
    float dt;
    float dh;

    rt = rand() % 3;
    rh = rand() % 3;

    dt = (rt - 1) * 0.5f;
    dh = (rh - 1) * 0.5f;

    g_state.temp += dt;
    g_state.humi += dh;

    g_state.temp = clamp_float(g_state.temp, TEMP_MIN, TEMP_MAX);
    g_state.humi = clamp_float(g_state.humi, HUMI_MIN, HUMI_MAX);
}

/* ============================================================
 * 十八、自动控制算法
 * ============================================================ */

static void auto_control_update(void)
{
    if (!g_state.auto_mode) {
        return;
    }

    /*
     * 风扇自动调速。
     */
    if (g_state.temp < 26.0f) {
        g_state.fan_on = 0;
        g_state.fan_level = 0;
        g_state.fan_freq = 0;
    } else if (g_state.temp < 28.0f) {
        g_state.fan_on = 1;
        g_state.fan_level = 1;
        g_state.fan_freq = 1000;
    } else if (g_state.temp < 30.0f) {
        g_state.fan_on = 1;
        g_state.fan_level = 2;
        g_state.fan_freq = 2000;
    } else if (g_state.temp < 32.0f) {
        g_state.fan_on = 1;
        g_state.fan_level = 3;
        g_state.fan_freq = 3000;
    } else {
        g_state.fan_on = 1;
        g_state.fan_level = 4;
        g_state.fan_freq = 4000;
    }

    /* 空调自动控制，带回差 */
    if (g_state.temp >= AIR_ON_TEMP) {
        g_state.air_on = 1;
    } else if (g_state.temp < AIR_OFF_TEMP) {
        g_state.air_on = 0;
    }

    /* 窗帘自动控制 */
    if (g_state.temp >= CURTAIN_ON_TEMP) {
        g_state.curtain_on = 1;
    } else if (g_state.temp < CURTAIN_OFF_TEMP) {
        g_state.curtain_on = 0;
    }
}

/* 前向声明：handle_touch_click() 内部调用 handle_key_code() */
static void handle_key_code(int code);

/* ============================================================
 * 十九、触摸输入处理
 * ============================================================ */

static int get_abs_range(int fd, int code, int *min, int *max)
{
    struct input_absinfo absinfo;

    if (ioctl(fd, EVIOCGABS(code), &absinfo) == 0) {
        *min = absinfo.minimum;
        *max = absinfo.maximum;
        return 0;
    }

    return -1;
}

static int scale_value(int value, int in_min, int in_max, int out_max)
{
    if (in_max == in_min) {
        return value;
    }

    if (value < in_min) value = in_min;
    if (value > in_max) value = in_max;

    return (value - in_min) * out_max / (in_max - in_min);
}

static void handle_touch_click(int x, int y)
{
    printf("touch click: x=%d, y=%d\n", x, y);

    /* 退出区域 */
    if (in_rect(x, y, EXIT_X1, EXIT_Y1, EXIT_X2, EXIT_Y2)) {
        printf("点击 EXIT，程序退出。\n");
        led_all_off();
        pwm_stop();

        if (touch_fd >= 0) close(touch_fd);
        if (key_fd >= 0) close(key_fd);

        if (fb_mem) {
            munmap(fb_mem, fb_size);
            fb_mem = NULL;
        }

        if (fb_fd >= 0) close(fb_fd);

        exit(0);
    }

    /* 模式切换按钮 */
    if (in_rect(x, y, MODE_X1, MODE_Y1, MODE_X2, MODE_Y2)) {
        g_state.auto_mode = !g_state.auto_mode;

        printf("模式切换为：%s\n", g_state.auto_mode ? "自动模式" : "手动模式");

        if (g_state.auto_mode) {
            auto_control_update();
        }

        apply_outputs();

        /* 刷新：状态面板 + 模式按钮 + 受自动模式影响的设备按钮 */
        need_redraw_status   = 1;
        need_redraw_mode     = 1;
        need_redraw_fan      = 1;
        need_redraw_air      = 1;
        need_redraw_curtain  = 1;
        /* 卧室灯不受自动模式影响，不刷新 */
        return;
    }

    /*
     * 温湿度调节触摸按钮。
     * 直接复用 handle_key_code() 的逻辑（温湿度变化、自动控制、局部刷新）。
     */
    if (in_rect(x, y, TEMPUP_X1, TEMP_CTRL_Y1, TEMPUP_X2, TEMP_CTRL_Y2)) {
        printf("触摸 TEMP+\n");
        handle_key_code(KEY_K3_TEMP_UP);
        return;
    }
    if (in_rect(x, y, TEMPDOWN_X1, TEMP_CTRL_Y1, TEMPDOWN_X2, TEMP_CTRL_Y2)) {
        printf("触摸 TEMP-\n");
        handle_key_code(KEY_K4_TEMP_DOWN);
        return;
    }
    if (in_rect(x, y, HUMIUP_X1, TEMP_CTRL_Y1, HUMIUP_X2, TEMP_CTRL_Y2)) {
        printf("触摸 HUMI+\n");
        handle_key_code(KEY_K6_HUMI_UP);
        return;
    }
    if (in_rect(x, y, HUMIDOWN_X1, TEMP_CTRL_Y1, HUMIDOWN_X2, TEMP_CTRL_Y2)) {
        printf("触摸 HUMI-\n");
        handle_key_code(KEY_K2_HUMI_DOWN);
        return;
    }

    /* 开关 1：风扇 */
    if (in_rect(x, y, FAN_X1, BTN_Y1, FAN_X2, BTN_Y2)) {
        if (!g_state.auto_mode) {
            g_state.fan_on = !g_state.fan_on;

            if (g_state.fan_on) {
                g_state.fan_level = 2;
                g_state.fan_freq = 2000;
            } else {
                g_state.fan_level = 0;
                g_state.fan_freq = 0;
            }

            printf("手动切换风扇：%s\n", g_state.fan_on ? "ON" : "OFF");
            apply_outputs();

            need_redraw_status = 1;
            need_redraw_fan    = 1;
        } else {
            printf("自动模式下风扇由温度控制。\n");
        }

        return;
    }

    /* 开关 2：空调 */
    if (in_rect(x, y, AIR_X1, BTN_Y1, AIR_X2, BTN_Y2)) {
        if (!g_state.auto_mode) {
            g_state.air_on = !g_state.air_on;
            printf("手动切换空调：%s\n", g_state.air_on ? "ON" : "OFF");
            apply_outputs();

            need_redraw_status = 1;
            need_redraw_air    = 1;
        } else {
            printf("自动模式下空调由温度阈值控制。\n");
        }

        return;
    }

    /* 开关 3：窗帘 */
    if (in_rect(x, y, CURTAIN_X1, BTN_Y1, CURTAIN_X2, BTN_Y2)) {
        if (!g_state.auto_mode) {
            g_state.curtain_on = !g_state.curtain_on;
            printf("手动切换窗帘：%s\n", g_state.curtain_on ? "ON" : "OFF");
            apply_outputs();

            need_redraw_status    = 1;
            need_redraw_curtain   = 1;
        } else {
            printf("自动模式下窗帘由温度阈值控制。\n");
        }

        return;
    }

    /* 开关 4：卧室灯（手动/自动模式都允许触摸切换） */
    if (in_rect(x, y, LIGHT_X1, BTN_Y1, LIGHT_X2, BTN_Y2)) {
        g_state.light_on = !g_state.light_on;
        printf("切换卧室灯：%s\n", g_state.light_on ? "ON" : "OFF");
        apply_outputs();

        need_redraw_status = 1;
        need_redraw_light  = 1;
        return;
    }
}

static void handle_touch_events(void)
{
    static int raw_x = 0;
    static int raw_y = 0;
    static int touch_down = 0;
    static int last_touch_down = 0;

    static int x_min = 0;
    static int x_max = SCREEN_W - 1;
    static int y_min = 0;
    static int y_max = SCREEN_H - 1;
    static int range_inited = 0;

    struct input_event ev;

    if (!range_inited) {
        get_abs_range(touch_fd, ABS_X, &x_min, &x_max);
        get_abs_range(touch_fd, ABS_Y, &y_min, &y_max);

        printf("Touch device: %s\n", TOUCH_DEV);
        printf("ABS_X range: %d ~ %d\n", x_min, x_max);
        printf("ABS_Y range: %d ~ %d\n", y_min, y_max);

        range_inited = 1;
    }

    while (read(touch_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
                raw_x = ev.value;
            } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                raw_y = ev.value;
            }
        }

        if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            touch_down = ev.value;
        }

        if (ev.type == EV_SYN) {
            int sx;
            int sy_raw_scaled;
            int sy;

            sx = scale_value(raw_x, x_min, x_max, SCREEN_W - 1);
            sy_raw_scaled = scale_value(raw_y, y_min, y_max, SCREEN_H - 1);

            /* Y 轴翻转：触摸原点左下角 → 显示原点左上角 */
            sy = SCREEN_H - 1 - sy_raw_scaled;

            /* 松手时触发一次点击，避免长按重复触发 */
            if (last_touch_down == 1 && touch_down == 0) {
                handle_touch_click(sx, sy);
            }

            last_touch_down = touch_down;
        }
    }
}

/* ============================================================
 * 二十、按键处理
 * ============================================================ */

static void handle_key_code(int code)
{
    int changed = 0;
    int old_fan_on, old_air_on, old_curtain_on;
    int old_fan_level, old_fan_freq;

    printf("KEY pressed: code=%d\n", code);

    /* 记录旧状态，用于判断哪些设备状态变化 */
    old_fan_on     = g_state.fan_on;
    old_fan_level  = g_state.fan_level;
    old_fan_freq   = g_state.fan_freq;
    old_air_on     = g_state.air_on;
    old_curtain_on = g_state.curtain_on;

    if (code == KEY_K3_TEMP_UP) {
        g_state.temp += 0.5f;
        changed = 1;
        printf("K3: TEMP +0.5 -> %.1f\n", g_state.temp);
    } else if (code == KEY_K4_TEMP_DOWN) {
        g_state.temp -= 0.5f;
        changed = 1;
        printf("K4: TEMP -0.5 -> %.1f\n", g_state.temp);
    } else if (code == KEY_K6_HUMI_UP) {
        g_state.humi += 0.5f;
        changed = 1;
        printf("K6: HUMI +0.5 -> %.1f\n", g_state.humi);
    } else if (code == KEY_K2_HUMI_DOWN) {
        g_state.humi -= 0.5f;
        changed = 1;
        printf("K2: HUMI -0.5 -> %.1f\n", g_state.humi);
    }

    g_state.temp = clamp_float(g_state.temp, TEMP_MIN, TEMP_MAX);
    g_state.humi = clamp_float(g_state.humi, HUMI_MIN, HUMI_MAX);

    if (changed) {
        if (g_state.auto_mode) {
            auto_control_update();
        }

        apply_outputs();

        /* 状态面板总是需要刷新（温湿度变化了） */
        need_redraw_status = 1;

        /* 风扇状态或频率变化 → 刷新风扇按钮 */
        if (g_state.fan_on != old_fan_on ||
            g_state.fan_level != old_fan_level ||
            g_state.fan_freq != old_fan_freq) {
            need_redraw_fan = 1;
        }

        /* 空调状态变化 → 刷新空调按钮 */
        if (g_state.air_on != old_air_on) {
            need_redraw_air = 1;
        }

        /* 窗帘状态变化 → 刷新窗帘按钮 */
        if (g_state.curtain_on != old_curtain_on) {
            need_redraw_curtain = 1;
        }
    }
}

static void handle_key_events(void)
{
    struct input_event ev;

    while (read(key_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_KEY) {
            if (ev.value == 1) {
                handle_key_code(ev.code);
            }
        }
    }
}

/* ============================================================
 * 二十一、初始化与退出
 * ============================================================ */

static int set_nonblock(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int init_inputs(void)
{
    touch_fd = open(TOUCH_DEV, O_RDONLY);
    if (touch_fd < 0) {
        perror("open touch device failed");
        return -1;
    }

    key_fd = open(KEY_DEV, O_RDONLY);
    if (key_fd < 0) {
        perror("open key device failed");
        close(touch_fd);
        touch_fd = -1;
        return -1;
    }

    set_nonblock(touch_fd);
    set_nonblock(key_fd);

    printf("Touch input: %s\n", TOUCH_DEV);
    printf("Key input  : %s\n", KEY_DEV);

    return 0;
}

static void cleanup(void)
{
    pwm_stop();
    led_all_off();

    if (touch_fd >= 0) {
        close(touch_fd);
        touch_fd = -1;
    }

    if (key_fd >= 0) {
        close(key_fd);
        key_fd = -1;
    }

    if (fb_mem) {
        munmap(fb_mem, fb_size);
        fb_mem = NULL;
    }

    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }
}

/* ============================================================
 * 二十二、主函数
 * ============================================================ */

int main(void)
{
    time_t last_sensor_update;
    time_t now;

    srand(time(NULL));

    /*
     * 初始化系统状态。
     */
    memset(&g_state, 0, sizeof(g_state));

    g_state.temp = INIT_TEMP;
    g_state.humi = INIT_HUMI;

    g_state.auto_mode = 0;

    g_state.fan_on = 0;
    g_state.air_on = 0;
    g_state.curtain_on = 0;
    g_state.light_on = 0;

    g_state.fan_level = 0;
    g_state.fan_freq = 0;

    /*
     * 初始化底层设备。
     */
    if (init_fb() < 0) {
        return 1;
    }

    led_init_all();

    if (pwm_init() < 0) {
        printf("PWM2 初始化失败，风扇功能可能不可用。\n");
    }

    if (init_inputs() < 0) {
        cleanup();
        return 1;
    }

    /* 初始输出全部关闭 */
    apply_outputs();

    /*
     * 启动时完整绘制一次静态 UI。
     */
    draw_static_ui();

    /*
     * 启动时所有动态脏标记置 1，触发首次全绘制。
     */
    need_redraw_status   = 1;
    need_redraw_mode     = 1;
    need_redraw_fan      = 1;
    need_redraw_air      = 1;
    need_redraw_curtain  = 1;
    need_redraw_light    = 1;
    need_redraw_name_id  = 1;

    flush_dirty();

    printf("系统启动完成。\n");
    printf("前四个开关在底部一排：FAN / AIR / CURT / LAMP。\n");
    printf("中间按钮切换 MANUAL / AUTO。\n");
    printf("K3/K4 调节温度，K6/K2 调节湿度。\n");
    printf("右上角显示姓名：冯威成，学号：2023020518。\n");
    printf("如果 K2/K6 不生效，请查看终端打印的 KEY pressed: code=xxx，然后修改代码顶部宏。\n");

    last_sensor_update = time(NULL);

    /*
     * 主循环：
     *   1. select() 同时监听触摸和按键；
     *   2. 每 2 秒模拟温湿度随机变化；
     *   3. 自动模式下更新设备状态；
     *   4. 末尾统一根据 dirty flag 刷新局部区域。
     */
    while (1) {
        fd_set rfds;
        struct timeval tv;
        int maxfd;
        int ret;

        FD_ZERO(&rfds);
        FD_SET(touch_fd, &rfds);
        FD_SET(key_fd, &rfds);

        maxfd = touch_fd > key_fd ? touch_fd : key_fd;

        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        if (ret > 0) {
            if (FD_ISSET(touch_fd, &rfds)) {
                handle_touch_events();
            }

            if (FD_ISSET(key_fd, &rfds)) {
                handle_key_events();
            }
        }

        now = time(NULL);

        /*
         * 每 2 秒模拟一次温湿度变化。
         */
        if (now - last_sensor_update >= 2) {
            int old_fan_on, old_air_on, old_curtain_on;
            int old_fan_level, old_fan_freq;

            old_fan_on     = g_state.fan_on;
            old_fan_level  = g_state.fan_level;
            old_fan_freq   = g_state.fan_freq;
            old_air_on     = g_state.air_on;
            old_curtain_on = g_state.curtain_on;

            last_sensor_update = now;
            sensor_sim_update_random();

            if (g_state.auto_mode) {
                auto_control_update();
            }

            apply_outputs();

            /* 温湿度变化，刷新状态面板 */
            need_redraw_status = 1;

            /* 仅当设备状态真正变化时才刷新对应按钮 */
            if (g_state.fan_on != old_fan_on ||
                g_state.fan_level != old_fan_level ||
                g_state.fan_freq != old_fan_freq) {
                need_redraw_fan = 1;
            }

            if (g_state.air_on != old_air_on) {
                need_redraw_air = 1;
            }

            if (g_state.curtain_on != old_curtain_on) {
                need_redraw_curtain = 1;
            }

            printf("TEMP=%.1f, HUMI=%.1f, MODE=%s, FAN=%s L%d %dHz, AIR=%s, CURT=%s, LAMP=%s\n",
                   g_state.temp,
                   g_state.humi,
                   g_state.auto_mode ? "AUTO" : "MANUAL",
                   g_state.fan_on ? "ON" : "OFF",
                   g_state.fan_level,
                   g_state.fan_freq,
                   g_state.air_on ? "ON" : "OFF",
                   g_state.curtain_on ? "ON" : "OFF",
                   g_state.light_on ? "ON" : "OFF");
        }

        /* 末尾统一根据 dirty flag 刷新局部区域 */
        flush_dirty();
    }

    cleanup();
    return 0;
}
