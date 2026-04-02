CXX = i686-w64-mingw32-g++
TARGET = d3d9.dll
SRCS = patch.cpp
DEFS = d3d9.def

CXXFLAGS = -Ofast \
           -flto \
           -mfpmath=sse \
           -msse2 \
           -fno-rtti \
           -fno-exceptions

LDFLAGS = -shared \
          -s \
          -static \
          -static-libgcc \
          -static-libstdc++ \
          -Wl,--enable-stdcall-fixup \
          -mwindows

all: $(TARGET)

$(TARGET): $(SRCS) $(DEFS)
	$(CXX) -o $@ $(SRCS) $(DEFS) $(CXXFLAGS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean