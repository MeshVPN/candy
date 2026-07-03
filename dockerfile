FROM alpine AS base
RUN apk update
RUN apk add openssl poco

FROM base AS build
RUN apk add git cmake ninja pkgconf g++ openssl-dev poco-dev linux-headers
# 用户态协议栈编译开关，默认 OFF：官方发行镜像零影响。
# 回归/需要 userspace 落地时用 --build-arg CANDY_NETSTACK=ON 构建。
ARG CANDY_NETSTACK=OFF
COPY . candy
RUN cd candy && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCANDY_NETSTACK=${CANDY_NETSTACK} && cmake --build build && cmake --install build

FROM base AS product
RUN install -D /dev/null /var/lib/candy/lost
COPY --from=build /usr/local/bin/candy /usr/bin/candy
COPY candy.cfg /etc/candy.cfg
ENTRYPOINT ["/usr/bin/candy"]
CMD ["-c", "/etc/candy.cfg"]
