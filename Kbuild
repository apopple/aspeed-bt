GIT_VERSION := $(shell git --work-tree=$M --git-dir=$M/.git describe --always --long --dirty || echo unknown)
ccflags-y += -DGIT_VERSION=\"${GIT_VERSION}\"
obj-m	:= bt-host.o
