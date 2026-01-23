# 告诉 L4Re 去哪里找 Fiasco 内核和你写的 modules.list
export MODULE_SEARCH_PATH="$(pwd)/build/kernel:$(pwd)/conf"

# 进入 L4Re 的构建目录
cd l4re/build_arm64

# 开始打包 (指定 E=fiasco-base-test 匹配我们刚写的配方名)
make E=fiasco-base-test elfimage

# 将生成的镜像复制到根目录的 build 文件夹中，方便使用
cp images/bootstrap.elf ../../build/
cd ../../