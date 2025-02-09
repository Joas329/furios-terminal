#!/bin/bash

# Copyright 2021 Johannes Marbach
#
# This file is part of furios-terminal, hereafter referred to as the program.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.


cd squeek2lvgl
pipenv install
pipenv run python squeek2lvgl.py \
    --input us.yaml \
    --input de.yaml \
    --input es.yaml \
    --input fr.yaml \
    --output .. \
    --surround-space-with-arrows \
    --shift-keycap '\xef\x8d\x9b'
