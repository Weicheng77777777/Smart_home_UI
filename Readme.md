# **温湿度智能控制系统（GEC6818 / Linux framebuffer）**

这是一个基于嵌入式 Linux framebuffer、触摸屏、按键、PWM、LED 的课程设计项目，用来实现一个“温湿度智能控制系统”的演示界面和底层控制逻辑。

本项目已经在 **64 位 GEC6818 开发板** 上跑通，系统为 **Weicheng 自行移植与适配的 Ubuntu 16.04.6 LTS**，属于当前项目的核心运行环境之一。也就是说，这不是完全照搬学校提供环境的“原样例程”，而是在真实开发板和真实系统环境上完成的一套完整适配版本。
<img width="1707" height="1280" alt="a4fb3c407e5bfa4193a71eb9cccb9037" src="https://github.com/user-attachments/assets/f262c7b4-5dd5-453e-a4ef-689df41411c3" />

---

## **1. 项目特色**

这个项目不是单纯的“图片切换 UI”，而是把下面这些内容真正串起来了：

- framebuffer 图形界面显示
- 电容/电阻触摸屏点击控制
- Linux input 按键事件读取
- PWM 输出模拟风扇
- LED 设备模拟继电器控制空调、窗帘、卧室灯
- 自动模式 / 手动模式切换
- 温湿度动态显示
- 温度变化驱动风扇转速变化
- 本地姓名、学号点阵显示
- 针对嵌入式小屏幕做的局部刷新优化，尽量减少闪屏

---

## **2. 本项目运行环境**

### **2.1 硬件平台**

本项目当前验证通过的平台是：

- **GEC6818
- **64 位 ARM（aarch64）**
- 屏幕分辨率：**800 × 480**
- framebuffer：`/dev/fb0`
- 触摸屏输入：`/dev/input/event0`
- 按键输入：`/dev/input/event3`

### **2.2 操作系统**

系统信息如下：

- **Ubuntu 16.04.6 LTS**
- Linux kernel：**4.4.172-s5p6818**
- 架构：**aarch64**

这套系统不是常见的原版发行镜像，而是 **Weicheng 移植并适配到这块 64 位 GEC6818 板子上的系统**。因此，项目里很多底层节点、行为、输入输出特征，都是围绕这套系统实际测试后写出来的。

这也是本项目“独一无二”的地方：  
**它不是只在 PPT 里成立，而是在 Weicheng 移植的 64 位 GEC6818 Linux 系统上真正运行过。**

---

## **3. 项目实现了什么**

项目界面主要包括：

- 顶部标题栏
- 温湿度状态显示区
- 中间一排 5 个按钮
- 底部一排 4 个设备开关
- 右上角姓名学号点阵显示

### **3.1 五个中间按钮**
中间一排横向排列 5 个按钮：

- `T-`：温度减小
- `T+`：温度增大
- `MANUAL / AUTO`：模式切换
- `H-`：湿度减小
- `H+`：湿度增大

### **3.2 底部四个设备按钮**
底部一排对称排列 4 个设备控制按钮：

- `FAN`：风扇
- `AIR`：空调
- `CURT`：窗帘
- `LAMP`：卧室灯

### **3.3 两种模式**
项目支持两种工作模式：

#### **手动模式**
用户通过触摸按钮直接控制：

- 风扇开关
- 空调开关
- 窗帘开关
- 卧室灯开关

同时也可以通过中间按钮或实体按键修改模拟温湿度值。

#### **自动模式**
系统根据当前温度自动控制设备：

- 温度越高，风扇 PWM 频率越高
- 温度高到一定阈值时，自动打开空调
- 温度更高时，可自动触发窗帘执行器逻辑
- 卧室灯不参与自动控制，始终允许手动操作

---

## **4. 为什么这个项目和学校 PPT 不完全一样**

这个问题非常重要，必须说明白。

### **4.1 因为硬件条件不完全一致**
学校 PPT 里的方案通常是假设所有外设都齐全，例如：

- DHT11 温湿度模块
- 电机模块
- 继电器模块
- 风扇模块
- 水泵模块
- 标准驱动接口完全一致

但现实里，开发板到手后，往往会遇到这些情况：

- 没有 DHT11
- 没有真实风扇
- 没有真实继电器
- 驱动节点与 PPT 示例不一致
- 学校不同批次板子、不同系统版本，接口路径也不同

### **4.2 所以 Weicheng 做了合理替代**
为了让项目能在现有硬件上完整跑通，本项目做了以下替代与扩展：

#### **1）没有 DHT11，于是改为软件模拟**
学校 PPT 中温湿度传感器可能使用 DHT11，但当前硬件环境没有这个模块，所以本项目采用：

- 初始温度：约 `33.0°C`
- 初始湿度：约 `60.0%`
- 定时随机波动
- 支持手动加减

这样既能模拟真实环境变化，又能用于测试自动控制逻辑。

#### **2）风扇不用学校那种简单 0/1 电机，而改成 PWM 算法**
学校方案里风扇常常只是：

- 开
- 关

但本项目因为板上更适合用 PWM2 驱动蜂鸣器/执行器，所以做了一个更有展示效果的扩展：

- 温度越高
- PWM 频率越高
- 风扇“速度”越快

这是一个比单纯 0/1 电机开关更丰富的实现，也更适合展示自动控制算法。

#### **3）继电器效果由 LED 模拟**
有些学校板卡上没有单独的继电器模块，或者接线不方便，所以这里把：

- 空调
- 窗帘
- 卧室灯

这些设备的控制逻辑，先映射到板子上的 LED 设备节点来模拟，便于演示与调试。

也就是说：

- UI 是真实的
- 控制逻辑是真实的
- 自动模式是真实的
- 底层输出是真实的
- 只是某些硬件执行器由现有 LED / PWM 资源来代替

这种做法非常适合课程设计和开源学习。

---

## **5. 当前项目使用的底层接口**

在 Weicheng 当前这块 GEC6818 板子上，已经确认的接口如下。

### **5.1 framebuffer**
```bash
/dev/fb0
```

### **5.2 触摸屏**
```bash
/dev/input/event0
```

### **5.3 按键**
```bash
/dev/input/event3
```

按键 code 已确认：

- `K4 = 114`：温度 -0.5
- `K3 = 115`：温度 +0.5
- `K2 = 28`：湿度 -0.5
- `K6 = 59`：湿度 +0.5

### **5.4 PWM2**
```bash
/sys/class/pwm/pwmchip0/pwm2
```

### **5.5 LED 设备**
```bash
/sys/class/leds/gec6818:d7
/sys/class/leds/gec6818:d8
/sys/class/leds/gec6818:d9
/sys/class/leds/gec6818:d10
```

当前项目分配如下：

- `d7`：空调
- `d8`：窗帘
- `d9`：自动模式指示灯
- `d10`：卧室灯

---

## **6. 小白最关心：这个程序怎么移植到学校的开发板上？**

这是整个开源项目最重要的部分之一。

结论先说：

**这个项目可以移植，但不能直接无脑复制。最关键的是先查清楚学校那块板子的底层接口，然后再让 AI 帮你“移花接木”改代码。**

也就是说，迁移不是“重写一遍”，而是：

1. 先识别学校板子的底层设备节点；
2. 再把本项目里对应路径替换掉；
3. 再根据实际驱动行为微调逻辑。

---

## **7. 移植前必须先查的 5 类底层接口**

如果你想把项目移植到你学校实验室的开发板，请先查清楚下面这 5 类东西：

### **7.1 屏幕 framebuffer 是什么**
常见可能是：

```bash
/dev/fb0
```

但也可能不是。

### **7.2 触摸屏输入节点是什么**
常见是：

```bash
/dev/input/event0
```

但不同板子未必一样。

### **7.3 按键输入节点是什么**
常见可能是：

```bash
/dev/input/event1
/dev/input/event2
/dev/input/event3
```

必须实际查。

### **7.4 LED / 继电器节点是什么**
可能有三种情况：

#### **情况 A：LED 子系统**
```bash
/sys/class/leds/xxx/brightness
```

#### **情况 B：GPIO sysfs**
```bash
/sys/class/gpio/export
/sys/class/gpio/gpioXX/value
```

#### **情况 C：驱动节点**
```bash
/dev/led_ctl
/dev/relay_ctl
```

### **7.5 PWM 节点是什么**
可能是：

```bash
/sys/class/pwm/pwmchip0/pwm2
```

也可能是别的 `pwmchip`，甚至根本不是 sysfs，而是驱动节点。

---

## **8. 小白照着跑：如何读取学校开发板的底层接口**

下面这些命令非常重要。  
你只要把输出结果复制给 AI，就能大幅降低移植难度。

### **8.1 查看系统信息**
```bash
uname -a
cat /etc/os-release
uname -m
```

### **8.2 查看输入设备**
```bash
cat /proc/bus/input/devices
ls -l /dev/input/
```

### **8.3 查看 GPIO**
```bash
ls -l /sys/class/gpio
for c in /sys/class/gpio/gpiochip*; do
    echo "===== $c ====="
    echo -n "label: "; cat $c/label 2>/dev/null
    echo -n "base : "; cat $c/base 2>/dev/null
    echo -n "ngpio: "; cat $c/ngpio 2>/dev/null
done
```

### **8.4 查看 LED**
```bash
ls -l /sys/class/leds
for l in /sys/class/leds/*; do
    echo "===== $l ====="
    echo -n "brightness: "; cat $l/brightness 2>/dev/null
    echo -n "max_brightness: "; cat $l/max_brightness 2>/dev/null
    echo -n "trigger: "; cat $l/trigger 2>/dev/null
done
```

### **8.5 查看 PWM**
```bash
ls -l /sys/class/pwm
for p in /sys/class/pwm/pwmchip*; do
    echo "===== $p ====="
    echo -n "npwm: "; cat $p/npwm 2>/dev/null
    ls -l $p
done
```

### **8.6 查看 `/dev` 下是否有专用驱动节点**
```bash
ls -l /dev | grep -Ei "led|gpio|pwm|relay|motor|beep|buzzer|dht"
```

---

## **9. 如何确认触摸和按键到底是哪个 event 节点**

### **9.1 先看输入设备列表**
```bash
cat /proc/bus/input/devices
```

你要找类似这样的名字：

- `touchscreen`
- `gpio_keys`
- `keyboard`

### **9.2 再用小测试程序读 key code**
如果系统没有 `evtest`，就自己编译一个：

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>

int main(int argc, char *argv[])
{
    const char *dev = "/dev/input/event3";
    int fd;
    struct input_event ev;

    if (argc >= 2) dev = argv[1];

    fd = open(dev, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    while (1) {
        if (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_KEY) {
                printf("EV_KEY: code=%d, value=%d\n", ev.code, ev.value);
                fflush(stdout);
            }
        }
    }

    return 0;
}
```

编译：

```bash
gcc key_code_test.c -o key_code_test
chmod 777 key_code_test
./key_code_test /dev/input/eventX
```

然后按按键，记下 `code`。

---

## **10. 查完接口后，怎么借助 AI“移花接木”**

这是本项目很适合开源教学的一点。

你不需要完全会重写代码，只要做到下面三步：

### **第一步：把学校板子的真实接口查出来**
例如：

- 触摸：`/dev/input/event1`
- 按键：`/dev/input/event2`
- PWM：`/sys/class/pwm/pwmchip1/pwm0`
- LED：`/sys/class/leds/user-led0`
- LED：`/sys/class/leds/user-led1`

### **第二步：把这些结果告诉 AI**
比如你可以这样说：

> 这是我学校开发板的底层接口，请把原来 GEC6818 版本的 smart_home_ui.c 移植到这块板子：
> 触摸是 /dev/input/event1，按键是 /dev/input/event2，风扇 PWM 是 /sys/class/pwm/pwmchip1/pwm0，空调和窗帘分别用 /sys/class/leds/user-led0 和 /sys/class/leds/user-led1。

### **第三步：让 AI 帮你替换和改逻辑**
AI 重点会帮你做这些事：

- 替换设备节点路径
- 改按键 code
- 改 PWM 初始化逻辑
- 改 LED 控制函数
- 改 GPIO 或 relay 的写法
- 保留现有 UI 逻辑不变

这就是所谓的“移花接木”。

本项目开源的价值也在这里：  
**你不需要从 0 写一份，只要先查底层，再让 AI 把现有成熟逻辑嫁接过去。**

---

## **11. 编译与运行**

### **11.1 板子本机编译**
如果板子里有 `gcc`：

```bash
gcc smart_home_ui.c -o smart_home_ui
chmod 777 smart_home_ui
./smart_home_ui
```

### **11.2 交叉编译**
如果在 PC 上编译，请注意当前板子是 **64 位 aarch64**，所以要用：

```bash
aarch64-linux-gnu-gcc smart_home_ui.c -o smart_home_ui
```

不要误用 32 位 ARM 工具链。

---

## **12. 运行时如果屏幕被终端文字覆盖怎么办**

如果你发现 framebuffer UI 上叠加了 Ubuntu 终端文字，说明 Linux 控制台也在往 `/dev/fb0` 上输出。  
可以先尝试：

```bash
clear
setterm -cursor off
echo 0 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null
./smart_home_ui > /tmp/ui.log 2>&1
```

如果仍然有终端覆盖，再检查 framebuffer console：

```bash
for i in /sys/class/vtconsole/vtcon*; do
    echo "==== $i ===="
    cat $i/name
    cat $i/bind
done
```

如果 `vtcon1` 是 `frame buffer device`，可以临时解绑：

```bash
echo 0 > /sys/class/vtconsole/vtcon1/bind
./smart_home_ui > /tmp/ui.log 2>&1
```

退出后恢复：

```bash
echo 1 > /sys/class/vtconsole/vtcon1/bind
```

---

## **13. 本项目适合谁**

这个项目特别适合：

- 刚开始接触嵌入式 Linux 的同学
- 正在做课程设计的同学
- 想学 framebuffer UI 的同学
- 想把“UI + 触摸 + PWM + LED + 自动控制”串起来的同学
- 想借助 AI 做嵌入式移植的同学

---

## **14. 对小白最重要的一句话**

如果你只记住一句话，那就是：

**先不要急着改代码，先把你自己板子的底层接口查出来，再让 AI 帮你把本项目移植过去。**

因为真正难的不是界面，而是：

- 你的板子触摸到底是哪一个 `event`
- 你的按键 code 到底是多少
- 你的 PWM 到底挂在哪个 `pwmchip`
- 你的 LED / GPIO / relay 到底走哪个接口

这些一旦查清楚，剩下的工作就变得简单很多。

---

## **15. 鸣谢与说明**

本项目运行与适配基于：

- **64 位 GEC6818 开发板**
- **Weicheng 移植并适配的 Ubuntu 系统**
- 实际硬件条件下完成的替代实现与功能扩展

因此，它既是一个课程设计项目，也是一个非常适合继续二次开发、移植、教学演示的嵌入式 Linux 小项目。

---
