# Stage 1: Compilazione
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Installa il necessario per compilare
RUN apt-get update && apt-get install -y \
    gcc \
    libc6-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# Compilazione con map.c e map.h
RUN gcc -Wall server.c map.c -o server -lpthread
RUN gcc -Wall client.c map.c -o client -lpthread

# Stage 2: Runtime
FROM ubuntu:22.04

WORKDIR /app

# Copia solo i binari finali
COPY --from=builder /app/server .
COPY --from=builder /app/client .

# Crea i file per i volumi (evita che Docker crei directory al loro posto)
RUN touch users.txt score.txt filelog.txt

EXPOSE 8080
