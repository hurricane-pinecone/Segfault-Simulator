# Image for scripts/gcc-lint.sh: GCC/libstdc++ plus the SDL and GLEW dev headers
# the runtime includes, so a -fsyntax-only pass sees the same standard library and
# system headers as CI's Linux build. Built once and cached; rerun is instant.
FROM gcc:14
RUN apt-get update -qq \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev libsdl2-mixer-dev libglew-dev \
 && rm -rf /var/lib/apt/lists/*
