#!/bin/bash

# Убиваем старые процессы
pkill storage_client 2>/dev/null

# Очищаем временные хранилища
rm -rf storage_9001 storage_9002 storage_9003

# Сборка (если требуется)
cd build
cmake .. -DBUILD_TESTS=OFF
cmake --build . --target storage_client -j$(nproc)
cd ..

# Запускаем Storage-узлы (вывод подавляем)
./build/storage_client --server 9001 ./storage_9001 > /dev/null 2>&1 &
./build/storage_client --server 9002 ./storage_9002 > /dev/null 2>&1 &
sleep 1

# Запускаем Entrypoint (в фоне, лог пишем в файл)
./build/storage_client --entrypoint 8080 > entrypoint.log 2>&1 &
sleep 1

# Запускаем клиента в интерактивном режиме
./build/storage_client 127.0.0.1:8080