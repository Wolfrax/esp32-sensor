cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(env)

fatfs_create_rawflash_image(www storage/www FLASH_IN_PROJECT)