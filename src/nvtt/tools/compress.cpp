// Copyright NVIDIA Corporation 2007 -- Ignacio Castano <icastano@nvidia.com>
// 
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.

#include "cmdline.h"

#include <nvtt/nvtt.h>

#include <nvimage/Image.h>    // @@ It might be a good idea to use FreeImage directly instead of ImageIO.
#include <nvimage/ImageIO.h>
#include <nvimage/FloatImage.h>
#include <nvimage/DirectDrawSurface.h>

#include <nvcore/Ptr.h> // AutoPtr
#include <nvcore/StrLib.h> // Path
#include <nvcore/StdStream.h>
#include <nvcore/FileSystem.h>
#include <nvcore/Timer.h>


struct MyOutputHandler : public nvtt::OutputHandler
{
    MyOutputHandler(const char * name) : total(0), progress(0), percentage(0), stream(new nv::StdOutputStream(name)) {}
    virtual ~MyOutputHandler() { delete stream; }

    void setTotal(int64 t)
    {
        total = t + 128;
    }
    void setDisplayProgress(bool b)
    {
        verbose = b;
    }

    virtual void beginImage(int size, int width, int height, int depth, int face, int miplevel)
    {
        // ignore.
    }

    virtual void endImage()
    {
        // Ignore.
    }

    // Output data.
    virtual bool writeData(const void * data, int size)
    {
        nvDebugCheck(stream != NULL);
        stream->serialize(const_cast<void *>(data), size);

        progress += size;
        int p = int((100 * progress) / total);
        if (verbose && p != percentage)
        {
            nvCheck(p >= 0);

            percentage = p;
            printf("\r%d%%", percentage);
            fflush(stdout);
        }

        return true;
    }

    int64 total;
    int64 progress;
    int percentage;
    bool verbose;
    nv::StdOutputStream * stream;
};

struct MyErrorHandler : public nvtt::ErrorHandler
{
    virtual void error(nvtt::Error e)
    {
#if _DEBUG
        nvDebugBreak();
#endif
        printf("Error: '%s'\n", nvtt::errorString(e));
    }
};




// Set color to normal map conversion options.
void setColorToNormalMap(nvtt::InputOptions & inputOptions)
{
    inputOptions.setNormalMap(false);
    inputOptions.setConvertToNormalMap(true);
    inputOptions.setHeightEvaluation(1.0f/3.0f, 1.0f/3.0f, 1.0f/3.0f, 0.0f);
    //inputOptions.setNormalFilter(1.0f, 0, 0, 0);
    //inputOptions.setNormalFilter(0.0f, 0, 0, 1);
    inputOptions.setGamma(1.0f, 1.0f);
    inputOptions.setNormalizeMipmaps(true);
}

// Set options for normal maps.
void setNormalMap(nvtt::InputOptions & inputOptions)
{
    inputOptions.setNormalMap(true);
    inputOptions.setConvertToNormalMap(false);
    inputOptions.setGamma(1.0f, 1.0f);
    inputOptions.setNormalizeMipmaps(true);
}

// Set options for color maps.
void setColorMap(nvtt::InputOptions & inputOptions)
{
    inputOptions.setNormalMap(false);
    inputOptions.setConvertToNormalMap(false);
    inputOptions.setGamma(2.2f, 2.2f);
    inputOptions.setNormalizeMipmaps(false);
}



int main(int argc, char *argv[])
{
    MyAssertHandler assertHandler;
    MyMessageHandler messageHandler;

    bool alpha = false;
    bool normal = false;
    bool color2normal = false;
    bool wrapRepeat = false;
    bool noMipmaps = false;
    bool fast = false;
    bool nocuda = false;
    bool bc1n = false;
    bool luminance = false;
    nvtt::Format format = nvtt::Format_BC1;
    bool premultiplyAlpha = false;
    nvtt::MipmapFilter mipmapFilter = nvtt::MipmapFilter_Box;
    bool loadAsFloat = false;

    const char * externalCompressor = NULL;

    bool silent = false;
    bool dds10 = false;

    nv::Path input;
    nv::Path output;


    // Parse arguments.
    for (int i = 1; i < argc; i++)
    {
        // Input options.
        if (strcmp("-color", argv[i]) == 0)
        {
        }
        else if (strcmp("-alpha", argv[i]) == 0)
        {
            alpha = true;
        }
        else if (strcmp("-normal", argv[i]) == 0)
        {
            normal = true;
        }
        else if (strcmp("-tonormal", argv[i]) == 0)
        {
            color2normal = true;
        }
        else if (strcmp("-clamp", argv[i]) == 0)
        {
        }
        else if (strcmp("-repeat", argv[i]) == 0)
        {
            wrapRepeat = true;
        }
        else if (strcmp("-nomips", argv[i]) == 0)
        {
            noMipmaps = true;
        }
        else if (strcmp("-premula", argv[i]) == 0)
        {
            premultiplyAlpha = true;
        }
        else if (strcmp("-mipfilter", argv[i]) == 0)
        {
            if (i+1 == argc) break;
            i++;

            if (strcmp("box", argv[i]) == 0) mipmapFilter = nvtt::MipmapFilter_Box;
            else if (strcmp("triangle", argv[i]) == 0) mipmapFilter = nvtt::MipmapFilter_Triangle;
            else if (strcmp("kaiser", argv[i]) == 0) mipmapFilter = nvtt::MipmapFilter_Kaiser;
        }
        else if (strcmp("-float", argv[i]) == 0)
        {
            loadAsFloat = true;
        }

        // Compression options.
        else if (strcmp("-fast", argv[i]) == 0)
        {
            fast = true;
        }
        else if (strcmp("-nocuda", argv[i]) == 0)
        {
            nocuda = true;
        }
        else if (strcmp("-rgb", argv[i]) == 0)
        {
            format = nvtt::Format_RGB;
        }
        else if (strcmp("-lumi", argv[i]) == 0)
        {
            luminance = true;
            format = nvtt::Format_RGB;
        }
        else if (strcmp("-bc1", argv[i]) == 0)
        {
            format = nvtt::Format_BC1;
        }
        else if (strcmp("-bc1n", argv[i]) == 0)
        {
            format = nvtt::Format_BC1;
            bc1n = true;
        }
        else if (strcmp("-bc1a", argv[i]) == 0)
        {
            format = nvtt::Format_BC1a;
        }
        else if (strcmp("-bc2", argv[i]) == 0)
        {
            format = nvtt::Format_BC2;
        }
        else if (strcmp("-bc3", argv[i]) == 0)
        {
            format = nvtt::Format_BC3;
        }
        else if (strcmp("-bc3n", argv[i]) == 0)
        {
            format = nvtt::Format_BC3n;
        }
        else if (strcmp("-bc4", argv[i]) == 0)
        {
            format = nvtt::Format_BC4;
        }
        else if (strcmp("-bc5", argv[i]) == 0)
        {
            format = nvtt::Format_BC5;
        }

        // Undocumented option. Mainly used for testing.
        else if (strcmp("-ext", argv[i]) == 0)
        {
            if (i+1 < argc && argv[i+1][0] != '-') {
                externalCompressor = argv[i+1];
                i++;
            }
        }
        else if (strcmp("-pause", argv[i]) == 0)
        {
            printf("Press ENTER\n"); fflush(stdout);
            getchar();
        }

        // Output options
        else if (strcmp("-silent", argv[i]) == 0)
        {
            silent = true;
        }
        else if (strcmp("-dds10", argv[i]) == 0)
        {
            dds10 = true;
        }

        else if (argv[i][0] != '-')
        {
            input = argv[i];

            if (i+1 < argc && argv[i+1][0] != '-') {
                output = argv[i+1];
            }
            else
            {
                output.copy(input.str());
                output.stripExtension();
                output.append(".dds");
            }

            break;
        }
    }

    const uint version = nvtt::version();
    const uint major = version / 100 / 100;
    const uint minor = (version / 100) % 100;
    const uint rev = version % 100;


    printf("NVIDIA Texture Tools %u.%u.%u - Copyright NVIDIA Corporation 2007\n\n", major, minor, rev);

    if (input.isNull())
    {
        printf("usage: nvcompress [options] infile [outfile]\n\n");

        printf("Input options:\n");
        printf("  -color     \tThe input image is a color map (default).\n");
        printf("  -alpha     \tThe input image has an alpha channel used for transparency.\n");
        printf("  -normal    \tThe input image is a normal map.\n");
        printf("  -tonormal  \tConvert input to normal map.\n");
        printf("  -clamp     \tClamp wrapping mode (default).\n");
        printf("  -repeat    \tRepeat wrapping mode.\n");
        printf("  -nomips    \tDisable mipmap generation.\n");
        printf("  -premula   \tPremultiply alpha into color channel.\n");
        printf("  -mipfilter \tMipmap filter. One of the following: box, triangle, kaiser.\n");
        printf("  -float     \tLoad as floating point image.\n\n");

        printf("Compression options:\n");
        printf("  -fast    \tFast compression.\n");
        printf("  -nocuda  \tDo not use cuda compressor.\n");
        printf("  -rgb     \tRGBA format\n");
        printf("  -lumi    \tLUMINANCE format\n");
        printf("  -bc1     \tBC1 format (DXT1)\n");
        printf("  -bc1n    \tBC1 normal map format (DXT1nm)\n");
        printf("  -bc1a    \tBC1 format with binary alpha (DXT1a)\n");
        printf("  -bc2     \tBC2 format (DXT3)\n");
        printf("  -bc3     \tBC3 format (DXT5)\n");
        printf("  -bc3n    \tBC3 normal map format (DXT5nm)\n");
        printf("  -bc4     \tBC4 format (ATI1)\n");
        printf("  -bc5     \tBC5 format (3Dc/ATI2)\n\n");

        printf("Output options:\n");
        printf("  -silent  \tDo not output progress messages\n");
        printf("  -dds10   \tUse DirectX 10 DDS format\n\n");

        return EXIT_FAILURE;
    }

    // Make sure input file exists.
    if (!nv::FileSystem::exists(input.str()))
    {
        fprintf(stderr, "The file '%s' does not exist.\n", input.str());
        return 1;
    }

    // Set input options.
    nvtt::InputOptions inputOptions;

    if (nv::strCaseCmp(input.extension(), ".dds") == 0)
    {
        // Load surface.
        nv::DirectDrawSurface dds(input.str());
        if (!dds.isValid())
        {
            fprintf(stderr, "The file '%s' is not a valid DDS file.\n", input.str());
            return EXIT_FAILURE;
        }

        if (!dds.isSupported())
        {
            fprintf(stderr, "The file '%s' is not a supported DDS file.\n", input.str());
            return EXIT_FAILURE;
        }

        uint faceCount;
        if (dds.isTexture2D())
        {
            inputOptions.setTextureLayout(nvtt::TextureType_2D, dds.width(), dds.height());
            faceCount = 1;
        }
        else if (dds.isTexture3D())
        {
            inputOptions.setTextureLayout(nvtt::TextureType_3D, dds.width(), dds.height(), dds.depth());
            faceCount = 1;

            nvDebugBreak();
        }
        else 
        {
            nvDebugCheck(dds.isTextureCube());
            inputOptions.setTextureLayout(nvtt::TextureType_Cube, dds.width(), dds.height());
            faceCount = 6;
        }

        uint mipmapCount = dds.mipmapCount();

        nv::Image mipmap;

        for (uint f = 0; f < faceCount; f++)
        {
            for (uint m = 0; m < mipmapCount; m++)
            {
                dds.mipmap(&mipmap, f, m); // @@ Load as float.

                inputOptions.setMipmapData(mipmap.pixels(), mipmap.width(), mipmap.height(), mipmap.depth(), f, m);
            }
        }
    }
    else
    {
        if (nv::strCaseCmp(input.extension(), ".exr") == 0 || nv::strCaseCmp(input.extension(), ".hdr") == 0)
        {
            loadAsFloat = true;
        }

        if (loadAsFloat)
        {
            nv::AutoPtr<nv::FloatImage> image(nv::ImageIO::loadFloat(input.str()));

            if (image == NULL)
            {
                fprintf(stderr, "The file '%s' is not a supported image type.\n", input.str());
                return EXIT_FAILURE;
            }

            inputOptions.setFormat(nvtt::InputFormat_RGBA_32F);
            inputOptions.setTextureLayout(nvtt::TextureType_2D, image->width(), image->height());

            /*for (uint i = 0; i < image->componentNum(); i++)
            {
                inputOptions.setMipmapChannelData(image->channel(i), i, image->width(), image->height());
            }*/
        }
        else
        {
            // Regular image.
            nv::Image image;
            if (!image.load(input.str()))
            {
                fprintf(stderr, "The file '%s' is not a supported image type.\n", input.str());
                return 1;
            }

            inputOptions.setTextureLayout(nvtt::TextureType_2D, image.width(), image.height());
            inputOptions.setMipmapData(image.pixels(), image.width(), image.height());
        }
    }

    if (wrapRepeat)
    {
        inputOptions.setWrapMode(nvtt::WrapMode_Repeat);
    }
    else
    {
        inputOptions.setWrapMode(nvtt::WrapMode_Clamp);
    }

    if (alpha)
    {
        inputOptions.setAlphaMode(nvtt::AlphaMode_Transparency);
    }
    else
    {
        inputOptions.setAlphaMode(nvtt::AlphaMode_None);
    }

    // Block compressed textures with mipmaps must be powers of two.
    if (!noMipmaps && format != nvtt::Format_RGB)
    {
        inputOptions.setRoundMode(nvtt::RoundMode_ToPreviousPowerOfTwo);
    }

    if (normal)
    {
        setNormalMap(inputOptions);
    }
    else if (color2normal)
    {
        setColorToNormalMap(inputOptions);
    }
    else
    {
        setColorMap(inputOptions);
    }

    if (noMipmaps)
    {
        inputOptions.setMipmapGeneration(false);
    }

    /*if (premultiplyAlpha)
    {
        inputOptions.setPremultiplyAlpha(true);
        inputOptions.setAlphaMode(nvtt::AlphaMode_Premultiplied);
    }*/

    inputOptions.setMipmapFilter(mipmapFilter);

    nvtt::CompressionOptions compressionOptions;
    compressionOptions.setFormat(format);

    if (format == nvtt::Format_BC2) {
        // Dither alpha when using BC2.
        compressionOptions.setQuantization(/*color dithering*/false, /*alpha dithering*/true, /*binary alpha*/false);
    }
    else if (format == nvtt::Format_BC1a) {
        // Binary alpha when using BC1a.
        compressionOptions.setQuantization(/*color dithering*/false, /*alpha dithering*/true, /*binary alpha*/true, 127);
    }
    else if (format == nvtt::Format_RGBA)
    {
        if (luminance)
        {
            compressionOptions.setPixelFormat(8, 0xff, 0, 0, 0);
        }
        else {
            // @@ Edit this to choose the desired pixel format:
            // compressionOptions.setPixelType(nvtt::PixelType_Float);
            // compressionOptions.setPixelFormat(16, 16, 16, 16);
            // compressionOptions.setPixelType(nvtt::PixelType_UnsignedNorm);
            // compressionOptions.setPixelFormat(16, 0, 0, 0);
        }
    }

    if (fast)
    {
        compressionOptions.setQuality(nvtt::Quality_Fastest);
    }
    else
    {
        compressionOptions.setQuality(nvtt::Quality_Normal);
        //compressionOptions.setQuality(nvtt::Quality_Production);
        //compressionOptions.setQuality(nvtt::Quality_Highest);
    }

    if (bc1n)
    {
        compressionOptions.setColorWeights(1, 1, 0);
    }

    
    //compressionOptions.setColorWeights(0.2126, 0.7152, 0.0722);
    //compressionOptions.setColorWeights(0.299, 0.587, 0.114);
    //compressionOptions.setColorWeights(3, 4, 2);

    if (externalCompressor != NULL)
    {
        compressionOptions.setExternalCompressor(externalCompressor);
    }


    MyErrorHandler errorHandler;
    MyOutputHandler outputHandler(output.str());
    if (outputHandler.stream->isError())
    {
        fprintf(stderr, "Error opening '%s' for writting\n", output.str());
        return EXIT_FAILURE;
    }

    nvtt::Context context;
    context.enableCudaAcceleration(!nocuda);

    printf("CUDA acceleration ");
    if (context.isCudaAccelerationEnabled())
    {
        printf("ENABLED\n\n");
    }
    else
    {
        printf("DISABLED\n\n");
    }

    outputHandler.setTotal(context.estimateSize(inputOptions, compressionOptions));
    outputHandler.setDisplayProgress(!silent);

    nvtt::OutputOptions outputOptions;
    //outputOptions.setFileName(output);
    outputOptions.setOutputHandler(&outputHandler);
    outputOptions.setErrorHandler(&errorHandler);

    if (dds10)
    {
        outputOptions.setContainer(nvtt::Container_DDS10);
    }

    // printf("Press ENTER.\n");
    // fflush(stdout);
    // getchar();

    nv::Timer timer;
    timer.start();

    if (!context.process(inputOptions, compressionOptions, outputOptions))
    {
        return EXIT_FAILURE;
    }
    timer.stop();

    printf("\rtime taken: %.3f seconds\n", timer.elapsed());

    return EXIT_SUCCESS;
}

