#include "../include/record3d/Record3DStream.h"
#include "JPEGDecoder.h"
#include <lzfse.h>
#include <usbmuxd.h>
#include <array>

#define NTOHL_(n) (((((unsigned long)(n) & 0xFF)) << 24) | \
                  ((((unsigned long)(n) & 0xFF00)) << 8) | \
                  ((((unsigned long)(n) & 0xFF0000)) >> 8) | \
                  ((((unsigned long)(n) & 0xFF000000)) >> 24))

/// The public part
namespace Record3D
{
    Record3DStream::Record3DStream()
    {
        compressedDepthBuffer_ = new uint8_t[depthBufferSize_];
        lzfseScratchBuffer_ = new uint8_t[lzfse_decode_scratch_size()];

        depthImageBuffer_.resize( depthBufferSize_ );

        constexpr int numRGBChannels = 3;
        RGBImageBuffer_.resize( Record3DStream::FRAME_WIDTH * Record3DStream::FRAME_HEIGHT * sizeof( uint8_t ) * numRGBChannels );
    }

    Record3DStream::~Record3DStream()
    {
        delete[] lzfseScratchBuffer_;
        delete[] compressedDepthBuffer_;
    }

    std::vector<DeviceInfo> Record3DStream::GetConnectedDevices()
    {
        usbmuxd_device_info_t* deviceInfoList;
        int numDevices = usbmuxd_get_device_list( &deviceInfoList );
        std::vector<DeviceInfo> availableDevices;

        for ( int devIdx = 0; devIdx < numDevices; devIdx++ )
        {
            const auto &dev = deviceInfoList[ devIdx ];
            if ( dev.conn_type != CONNECTION_TYPE_USB ) continue;

            DeviceInfo currDevInfo;
            currDevInfo.handle = dev.handle;
            currDevInfo.productId = dev.product_id;
            currDevInfo.udid = std::string( dev.udid );

            availableDevices.push_back( currDevInfo );
        }

        usbmuxd_device_list_free( &deviceInfoList );

        return availableDevices;
    }


    bool Record3DStream::ConnectToDevice(const DeviceInfo &$device)
    {
        std::lock_guard<std::mutex> guard{ apiCallsMutex_ };

        // Do not reconnect if we are already streaming.
        if ( connectionEstablished_.load())
        { return false; }

        // Ensure we are indeed connected before continuing.
        auto socketNo = usbmuxd_connect( $device.handle, DEVICE_PORT );
        if ( socketNo < 0 )
        { return false; }

        // We are successfully connected, start runloop.
        connectionEstablished_.store( true );
        socketHandle_ = socketNo;

        // Create thread that is going to execute runloop.
        runloopThread_ = std::thread( [&]
                                      {
                                          StreamProcessingRunloop();
                                      } );
        runloopThread_.detach();
        return true;
    }

    void Record3DStream::Disconnect()
    {
        std::lock_guard<std::mutex> guard{ apiCallsMutex_ };

        connectionEstablished_.store( false );

        if ( onStreamStopped )
        {
            onStreamStopped();
        }
    }
}


/// The private part
namespace Record3D
{
    struct PeerTalkHeader
    {
        uint32_t a;
        uint32_t b;
        uint32_t c;
        uint32_t body_size;
    };

    struct Record3DHeader
    {
        uint32_t rgbSize;
        uint32_t depthSize;
    };

    void Record3DStream::StreamProcessingRunloop()
    {
        std::vector<uint8_t> rawMessageBuffer;
        rawMessageBuffer.resize( depthBufferSize_ * 2 ); // Overallocate to ensure there is always enough memory

        uint32_t numReceivedData = 0;

        while ( connectionEstablished_.load())
        {
            // 1. Receive the PeerTalk header
            PeerTalkHeader ptHeader;
            numReceivedData = ReceiveWholeBuffer( socketHandle_, (uint8_t*) &ptHeader, sizeof( ptHeader ));
            uint32_t messageBodySize = NTOHL_( ptHeader.body_size );
            if ( numReceivedData != sizeof( ptHeader ))
            { break; }

            // 2. Receive the whole body
            numReceivedData = ReceiveWholeBuffer( socketHandle_, (uint8_t*) rawMessageBuffer.data(),
                                                  messageBodySize );
            if ( numReceivedData != messageBodySize )
            { break; }

            // 3. Parse the body
            Record3DHeader record3DHeader;

            size_t offset = 0;
            size_t currSize = 0;

            // 3.1 Read the header of Record3D
            currSize = sizeof( Record3DHeader );
            memcpy((void*) &record3DHeader, rawMessageBuffer.data() + offset, currSize );
            offset += currSize;

            // 3.2 Read intrinsic matrix coefficients
            currSize = sizeof( IntrinsicMatrixCoeffs );
            memcpy((void*) &intrinsicMatrixCoeffs_, rawMessageBuffer.data() + offset, currSize );
            offset += currSize;

            // 3.3 Read and decode the RGB JPEG frame
            currSize = record3DHeader.rgbSize;
            int loadedWidth, loadedHeight, loadedChannels;
            uint8_t* rgbPixels = stbi_load_from_memory( rawMessageBuffer.data() + offset, currSize, &loadedWidth, &loadedHeight, &loadedChannels, STBI_rgb );
            memcpy( RGBImageBuffer_.data(), rgbPixels, RGBImageBuffer_.size());
            stbi_image_free( rgbPixels );
            offset += currSize;

            // 3.4 Read and decompress the depth frame
            currSize = record3DHeader.depthSize;
            DecompressDepthBuffer( rawMessageBuffer.data() + offset, currSize, depthImageBuffer_.data());

            if ( onNewFrame )
            {
#ifdef PYTHON_BINDINGS_BUILD
                onNewFrame();
#else
                onNewFrame( RGBImageBuffer_, depthImageBuffer_, FRAME_WIDTH, FRAME_HEIGHT, intrinsicMatrixCoeffs_ );
#endif
            }
        }

        Disconnect();
    }

    uint8_t* Record3DStream::DecompressDepthBuffer(const uint8_t* $compressedDepthBuffer, size_t $compressedDepthBufferSize, uint8_t* $destinationBuffer)
    {
        size_t outSize = lzfse_decode_buffer( $destinationBuffer, depthBufferSize_, $compressedDepthBuffer,
                                              $compressedDepthBufferSize, lzfseScratchBuffer_ );
        if ( outSize != depthBufferSize_ )
        {
#if DEBUG
            fprintf( stderr, "Decompression error!\n" );
#endif
            return nullptr;
        }
        return $destinationBuffer;
    }

    uint32_t Record3DStream::ReceiveWholeBuffer(int $socketHandle, uint8_t* $outputBuffer, uint32_t $numBytesToRead)
    {
        uint32_t numTotalReceivedBytes = 0;
        while ( numTotalReceivedBytes < $numBytesToRead )
        {
            uint32_t numRestBytes = $numBytesToRead - numTotalReceivedBytes;
            uint32_t numActuallyReceivedBytes = 0;
            if ( 0 != usbmuxd_recv( $socketHandle, (char*) ($outputBuffer + numTotalReceivedBytes), numRestBytes,
                                    &numActuallyReceivedBytes ))
            {
#if DEBUG
                fprintf( stderr, "ERROR WHILE RECEIVING DATA!\n" );
#endif
                return numTotalReceivedBytes;
            }
            numTotalReceivedBytes += numActuallyReceivedBytes;
        }

        return numTotalReceivedBytes;
    }
}
