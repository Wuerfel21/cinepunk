PATH=/c/msys64/mingw64/bin:$PATH g++ -static -Wall -O3 -mavx2 -mtune=znver1 -g -std=c++17 *.cpp src/*.cpp -o cinepunk.exe
