#!/bin/bash
#
# Copyright (C) 2024 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e

export ASM_JAR="${ANDROID_BUILD_TOP}/prebuilts/misc/common/asm/asm-9.6.jar"

# Move original classes to intermediate location.
mv $1 $1-intermediate-classes
mkdir $1

# Transform intermediate classes.
transformer_args="-cp ${ASM_JAR}:$PWD/transformer.jar transformer.ConstantTransformer"
for class in $1-intermediate-classes/*.class ; do
  transformed_class=$1/$(basename ${class})
  ${JAVA:-java} ${transformer_args} ${class} ${transformed_class}
done

