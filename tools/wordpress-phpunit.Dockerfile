FROM composer:2 AS composer
FROM php:8.3-cli-bookworm

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    bison \
    build-essential \
    ca-certificates \
    cmake \
    git \
    libavif-dev \
    libcrypt-dev \
    libfreetype6-dev \
    libjpeg62-turbo-dev \
    libncurses-dev \
    libpcre2-dev \
    libpng-dev \
    libssl-dev \
    libwebp-dev \
    libzip-dev \
    ninja-build \
    pkg-config \
    unzip \
    zlib1g-dev \
  && docker-php-ext-configure gd --with-avif --with-freetype --with-jpeg --with-webp \
  && docker-php-ext-install -j"$(nproc)" gd zip \
  && rm -rf /var/lib/apt/lists/*

COPY --from=composer /usr/bin/composer /usr/local/bin/composer
