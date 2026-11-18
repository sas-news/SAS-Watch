# T49 spike: SDK の chat/Noise 層を ESP-IDF 5.5.1 でビルドできるか
#
# SAS-Watch 本体には組み込まない独立プロジェクト。target は esp32s3
# (SAS-Watch と同じ)。SDK ミラーは読み取り専用で、必要なファイルだけ
# components/ にコピーしている (Apache-2.0、ヘッダ保持)。

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(muse_chat_spike)
