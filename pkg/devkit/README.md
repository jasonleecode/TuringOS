# TuringOS 应用开发包

用这个目录可以编译应用、把应用装进 QEMU 开发盘，并通过系统自带的 `app` 命令安装和运行。命令从仓库根目录执行。下面以 QEMU ARM virt 的默认 `native-shell` 镜像为目标。

## 构建和安装第一个应用

宿主机需要项目使用的 ARM 交叉工具链、GNU Make、Python 3、QEMU 和 e2fsprogs（`mkfs.ext4`、`debugfs`）。编译 WASM 还需要带 wasm32 后端的 Clang 和 wasm-ld。可以用 `WASM_CC`、`WASM_LD` 指定工具路径。

```sh
./build.sh --board virt all
./build.sh --board virt --pkg devkit/app --compile-only
pkg/devkit/tools/build-wasm pkg/devkit/app/hello_wasm.c build/devkit/hello.wasm
pkg/devkit/tools/mkdisk.py build/devkit/demo.img \
  --app greeting v1 build/l4re_virt/bin/arm_armv7a/l4f/sdk-hello data \
  --app portable v1 build/devkit/hello.wasm
./run_qemu_virt.sh --cfg native-shell --headless --no-net --disk-image build/devkit/demo.img
```

`mkdisk.py` 创建新盘，保留已有同名文件。每个 `--app` 接受应用名、版本、文件和可选权限。名称和版本各为 1 到 31 个 ASCII 字母、数字、下划线或连字符。开发盘接受 ARM32 小端 ELF 和 WASM v1，单文件上限 4 MiB。制盘时生成每个文件的 SHA-256，安装时由客体校验摘要和程序。后续启动复用同一个盘即可保留应用和数据。

在串口用 `root` / `12345678` 登录。执行 `cat /ext4/incoming/install.txt`，把其中两条完整的 `app install` 命令逐行输入 shell。宿主机制盘时也会打印相同的命令。安装完成后运行：

```sh
app status greeting
app run greeting | cat
app run greeting /data/message
cat /ext4/data/message
run app run portable sdk-arg
```

`greeting` 会在管道和文件中写出 `Hello from a TuringOS application`。`portable` 会打印 WASM 问候语和参数 `sdk-arg`，随后 shell 报告退出码 0。要试升级，可在制盘时再加一个同名、不同版本的 `--app`，在客体里依次安装两版，再用 `app rollback greeting` 切回上一版。

## 开发自己的原生应用

把示例的 C 源文件和 Makefile 复制到自己的目录，修改 Makefile 的 `TARGET` 为独有的程序名。已构建上述示例后，可以直接复用同一套头文件、库和板级编译参数：

```sh
mkdir -p ../my-turing-app
cp pkg/devkit/app/hello.c ../my-turing-app/
sed 's/^TARGET = sdk-hello$/TARGET = my-hello/' pkg/devkit/app/Makefile > ../my-turing-app/Makefile
pkg/devkit/tools/build-native --board virt ../my-turing-app
```

产物在 `build/l4re_virt/bin/arm_armv7a/l4f/my-hello`，可替换制盘命令中的 ELF 路径。使用自定义构建目录时，对 `build.sh` 和 `build-native` 设置相同的 `BUILD_OUT`。其他板型可先用对应的 `--board` 编译 `devkit/app`，再构建外部应用。上面的安装流程依赖 virt 镜像中的 ext4、spawnd 和应用管理工具。

模板在 `prog.mk` 前引入 `posix/app.mk`。把项目加入仓库的包注册表时，可参照示例的 `Control` 声明依赖。文件、管道和网络的可用范围见 [POSIX 接口说明](posix/README.md)。

## WASM

`build-wasm` 编译单个 freestanding C 源文件并导出 `main`。示例用 `env.puts` 输出文本，通过 `main(int argc, char **argv)` 接收参数。系统通过 WAMR 的 `wasm-run` 运行模块，提供内置导入，当前配置未启用 WASI。制盘时给 WASM 省略权限参数。[示例源码](app/hello_wasm.c) · [运行器](../wamr/runner/main.c)

## LVGL 图形应用

图形示例是一个独立的 ROM 程序。Ned 启动 `fb-drv`，并把显示 capability `fb` 交给示例。桌面环境中执行以下命令，QEMU 窗口中央应显示白色的 `Hello from TuringOS`。远程开发可在启动命令后追加 `--vnc 5901`。

```sh
ENTRY=devkit-lvgl ./build.sh --board virt all
./run_qemu_virt.sh --cfg devkit-lvgl --no-net --no-disk
```

开发自己的界面时，在 `frameworks/hello_lvgl.cc` 的 `create_ui` 中创建控件，`lv_port_run` 负责初始化显示、刷新和事件循环。当前 `app install` 提供的权限为 `net`、`data`、`tmp`，需要显示的应用由启动配置显式分配 `fb`。[LVGL 示例](frameworks/hello_lvgl.cc) · [图形启动配置](../../conf/devkit-lvgl.cfg)
