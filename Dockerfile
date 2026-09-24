FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends build-essential clang make ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN make -j4 && make test

FROM ubuntu:24.04
RUN useradd --system --uid 10001 --create-home kvstore
COPY --from=build /app/bin/kv-server /app/bin/kv-shard /app/bin/kv-proxy /app/bin/kv-client /usr/local/bin/
COPY --chmod=755 scripts/entrypoint.sh /entrypoint.sh
USER kvstore
EXPOSE 6379
ENTRYPOINT ["/entrypoint.sh"]
CMD ["kv-server"]
