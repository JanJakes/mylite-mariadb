FROM composer:2 AS composer
FROM php:8.3-cli-bookworm AS php-ext-builder

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

FROM php:8.3-cli-bookworm

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    ca-certificates \
    libavif15 \
    libfreetype6 \
    libjpeg62-turbo \
    libpng16-16 \
    libwebp7 \
    libzip4 \
    unzip \
  && rm -rf /var/lib/apt/lists/*

COPY --from=php-ext-builder /usr/local/lib/php/extensions/ /usr/local/lib/php/extensions/
COPY --from=php-ext-builder /usr/local/etc/php/conf.d/docker-php-ext-gd.ini /usr/local/etc/php/conf.d/
COPY --from=php-ext-builder /usr/local/etc/php/conf.d/docker-php-ext-zip.ini /usr/local/etc/php/conf.d/
COPY --from=composer /usr/bin/composer /usr/local/bin/composer
