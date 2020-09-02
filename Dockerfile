ARG ALPINE=alpine:3.12

FROM $ALPINE AS builder
WORKDIR /build
COPY main.c .
RUN apk add --no-cache gcc musl-dev linux-headers \
  && gcc -g main.c -o udp-broadcast-relay-redux

FROM $ALPINE
WORKDIR /runtime
COPY --from=builder /build/udp-broadcast-relay-redux .
ENTRYPOINT ["./udp-broadcast-relay-redux"]
