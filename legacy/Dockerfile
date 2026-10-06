FROM ubuntu:bionic

ENV DEBIAN_FRONTEND noninteractive
RUN apt-get -qq update
RUN apt-get -qqy upgrade

RUN apt-get -qqy install software-properties-common build-essential cpio linux-headers-generic

RUN ln -s /usr/src/linux-headers-$(uname -r)/include/generated/uapi/linux/version.h /usr/src/linux-headers-$(uname -r)/include/linux/version.h
COPY . /work
RUN make -C /work install

WORKDIR /work
