# jetson-ffmpeg
L4T Multimedia API for ffmpeg

**1.build and install library**

    git clone https://github.com/Keylost/jetson-ffmpeg.git
    cd jetson-ffmpeg
    mkdir build
    cd build
    cmake ..
    make
    sudo make install
    sudo ldconfig
	
**2.patch ffmpeg and build**

    clone one of supported ffmpeg versions
    git clone git://source.ffmpeg.org/ffmpeg.git -b release/4.2 --depth=1
    or
    git clone git://source.ffmpeg.org/ffmpeg.git -b release/4.4 --depth=1
    or
    git clone git://source.ffmpeg.org/ffmpeg.git -b release/6.0 --depth=1
    cd ffmpeg
    get and apply patch for your ffmpeg version
    wget -O ffmpeg_nvmpi.patch https://github.com/Keylost/jetson-ffmpeg/raw/master/ffmpeg_patches/ffmpeg4.2_nvmpi.patch
    or
    wget -O ffmpeg_nvmpi.patch https://github.com/Keylost/jetson-ffmpeg/raw/master/ffmpeg_patches/ffmpeg4.4_nvmpi.patch
    or
    wget -O ffmpeg_nvmpi.patch https://github.com/Keylost/jetson-ffmpeg/raw/master/ffmpeg_patches/ffmpeg6.0_nvmpi.patch
    git apply ffmpeg_nvmpi.patch
    ./configure --enable-nvmpi
    make
    sudo make install
    
**3.using**

### Supports Decoding
  - H.264/AVC (ffmpeg codec name: h264_nvmpi)
  - H.265/HEVC (ffmpeg codec name: hevc_nvmpi)
  - MPEG2 (ffmpeg codec name: mpeg2_nvmpi)
  - MPEG4 (ffmpeg codec name: mpeg4_nvmpi)
  - VP8 (ffmpeg codec name: vp8_nvmpi)
  - VP9 (ffmpeg codec name: vp9_nvmpi)
  
**example**

    ffmpeg -c:v h264_nvmpi -i input_file -f null -
	
### Supports Encoding
  - H.264/AVC (ffmpeg codec name: h264_nvmpi)
  - H.265/HEVC (ffmpeg codec name: hevc_nvmpi)
  
**example**

    ffmpeg -i input_file -c:v h264_nvmpi <output.mp4>
