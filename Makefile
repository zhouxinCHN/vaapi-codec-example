# VAAPI 硬件编解码 Demo - 顶层 Makefile
#
#   make            编译（自动生成 build/ 目录）
#   make install    安装到 ./install（可用 PREFIX=/usr/local 指定别处）
#   ./run           傻瓜式交互菜单
#   make clean      清理编译产物
#
# 也可直接使用:
#   make run        编译后进入交互菜单
#   make selftest   编译后跑一遍全流程自检

PREFIX     ?= $(CURDIR)/install
BUILD_DIR  ?= build
JOBS       ?= $(shell nproc 2>/dev/null || echo 4)
CMAKE      ?= cmake
BUILD_TYPE ?= Release

.PHONY: all configure build install run selftest clean check help

all: build

configure:
	@$(CMAKE) -S . -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX)

build: configure
	@$(CMAKE) --build $(BUILD_DIR) --parallel $(JOBS)

install: build
	@$(CMAKE) --install $(BUILD_DIR)
	@echo ""
	@echo "已安装到: $(PREFIX)"
	@echo "运行方式: $(PREFIX)/run   或   $(PREFIX)/bin/vaapi_demo info"

run: build
	@./run

selftest: build
	@./run selftest

clean:
	@rm -rf $(BUILD_DIR) $(PREFIX) output

help:
	@echo "make            编译"
	@echo "make install    安装到 $(PREFIX)   (可加 PREFIX=/usr/local)"
	@echo "make run        编译并启动交互菜单"
	@echo "make selftest   编译并执行全流程自检"
	@echo "make clean      清理 build/ install/ output/"
