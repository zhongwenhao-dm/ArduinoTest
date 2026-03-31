FROM 431983620723.dkr.ecr.ap-east-1.amazonaws.com/dm/common-images-bazel:5.1.1-ubuntu-20.04 as bazel

FROM 431983620723.dkr.ecr.ap-east-1.amazonaws.com/dm/common-images-clang-llvm:15.0.6-ubuntu-20.04 as clang-llvm

FROM ubuntu:20.04

# Set locale.
RUN apt-get update &&  \
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends locales && \
  localedef -i en_US -c -f UTF-8 -A /usr/share/locale/locale.alias en_US.UTF-8 && \
  rm -rf /var/lib/apt/lists/*
ENV LANG en_US.utf8

# Copy LLVM.
ENV PATH /opt/llvm/bin:$PATH
COPY --from=clang-llvm /data /opt
ENV LLVM_VERSION 15.0.6

# Copy Bazel.
COPY --from=bazel /usr/local/lib/bazel /usr/local/lib/bazel
RUN ln -s /usr/local/lib/bazel/bin/bazel /usr/local/bin/bazel

RUN apt-get update && \
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  git=1:2.25.1* \
  g++=4:9.3.0* \
  software-properties-common=0.99.9* \
  openjdk-11-jdk=11.0* \
  python3-dev=3.8.2* \
  python3-pip=20.0.2* && \
  rm -rf /var/lib/apt/lists/*

RUN pip3 install cpplint==1.6.0

RUN ldconfig
