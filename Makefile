#
# Makefile for packtool
#

O?=.
TARGET=$(O)/packtool
C_FILES=packtool.c

$(TARGET): $(C_FILES)
	gcc -o $@ $^ -lz -Wall
