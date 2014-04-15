    /*
 OFX GenericReader plugin.
 A base class for all OpenFX-based decoders.
 
 Copyright (C) 2013 INRIA
 Author Alexandre Gauthier-Foichat alexandre.gauthier-foichat@inria.fr
 
 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:
 
 Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 
 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.
 
 Neither the name of the {organization} nor the names of its
 contributors may be used to endorse or promote products derived from
 this software without specific prior written permission.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 
 INRIA
 Domaine de Voluceau
 Rocquencourt - B.P. 105
 78153 Le Chesnay Cedex - France
 
 */
#include "GenericReader.h"

#include <iostream>
#include <memory>
#include <climits>
#include <cmath>

#include "ofxsLog.h"
#include "ofxsCopier.h"

#ifdef OFX_EXTENSIONS_TUTTLE
#include <tuttle/ofxReadWrite.h>
#endif

#include "SequenceParsing/SequenceParsing.h"
#include "GenericOCIO.h"
#include "IOUtility.h"

// in the Reader context, the script name must be "filename", @see kOfxImageEffectContextReader
#define kReaderFileParamName "filename"
#define kReaderProxyFileParamName "proxy"
#define kReaderMissingFrameParamName "onMissingFrame"
#define kReaderFrameModeParamName "frameMode"
#define kReaderTimeOffsetParamName "timeOffset"
#define kReaderStartingFrameParamName "startingFrame"
#define kReaderOriginalFrameRangeParamName "ReaderOriginalFrameRangeParamName"


#define kReaderFirstFrameParamName "firstFrame"
#define kReaderLastFrameParamName "lastFrame"
#define kReaderBeforeParamName "before"
#define kReaderAfterParamName "after"

// if a hole in the sequence is larger than 2000 frames inside the sequence's time domain, this will output black frames.
#define MAX_SEARCH_RANGE 400000

GenericReaderPlugin::GenericReaderPlugin(OfxImageEffectHandle handle)
: OFX::ImageEffect(handle)
, _missingFrameParam(0)
, _outputClip(0)
, _fileParam(0)
, _proxyFileParam(0)
, _firstFrame(0)
, _beforeFirst(0)
, _lastFrame(0)
, _afterLast(0)
, _frameMode(0)
, _timeOffset(0)
, _startingFrame(0)
, _originalFrameRange(0)
, _ocio(new GenericOCIO(this))
, _settingFrameRange(false)
, _sequenceFromFiles(new SequenceParsing::SequenceFromFiles)
{
    _outputClip = fetchClip(kOfxImageEffectOutputClipName);
    
    _fileParam = fetchStringParam(kReaderFileParamName);
    _proxyFileParam = fetchStringParam(kReaderProxyFileParamName);
    _missingFrameParam = fetchChoiceParam(kReaderMissingFrameParamName);
    _firstFrame = fetchIntParam(kReaderFirstFrameParamName);
    _beforeFirst = fetchChoiceParam(kReaderBeforeParamName);
    _lastFrame = fetchIntParam(kReaderLastFrameParamName);
    _afterLast = fetchChoiceParam(kReaderAfterParamName);
    _frameMode = fetchChoiceParam(kReaderFrameModeParamName);
    _timeOffset = fetchIntParam(kReaderTimeOffsetParamName);
    _startingFrame = fetchIntParam(kReaderStartingFrameParamName);
    _originalFrameRange = fetchInt2DParam(kReaderOriginalFrameRangeParamName);
    
    ///set the values of the original range and the file param (and reparse the sequence)
    std::string filename;
    _fileParam->getValue(filename);
    
    try {
        if (!filename.empty()) {
            SequenceParsing::SequenceFromFiles::getSequenceOutOfFile(filename, _sequenceFromFiles);
            if (_sequenceFromFiles->isSingleFile()) {
                _originalFrameRange->setValue(0, 0);
            } else {
                _originalFrameRange->setValue(_sequenceFromFiles->getFirstFrame(), _sequenceFromFiles->getLastFrame());
            }
        }

    } catch(const std::exception& e) {
        setPersistentMessage(OFX::Message::eMessageError, "", e.what());
        return;
    }
}

GenericReaderPlugin::~GenericReaderPlugin(){
    delete _ocio;
}



bool GenericReaderPlugin::getTimeDomain(OfxRangeD &range) {

    bool ret = getSequenceTimeDomainInternal(range,false);
    if (ret) {
        timeDomainFromSequenceTimeDomain(range, false);
    }
    return ret;
}

bool GenericReaderPlugin::getSequenceTimeDomainInternal(OfxRangeD& range,bool canSetOriginalFrameRange) {
    
    ////first-off check if the original frame range param has valid values, in which
    ///case we don't bother calculating the frame range
    int originalMin,originalMax;
    _originalFrameRange->getValue(originalMin, originalMax);
    if (originalMin != INT_MIN && originalMax != INT_MAX) {
        range.min = originalMin;
        range.max = originalMax;
        return true;
    }
    
    ///otherwise compute the frame-range
    
    std::string filename;
    _fileParam->getValue(filename);
    ///call the plugin specific getTimeDomain (if it is a video-stream , it is responsible to
    ///find-out the time domain. If this function return false, it means this is an image sequence
    ///in which case our sequence parser will give us the sequence range
    if(!getSequenceTimeDomain(filename,range)){
        if (_sequenceFromFiles->isSingleFile()) {
            range.min = range.max = 0;
        } else {
            range.min = _sequenceFromFiles->getFirstFrame();
            range.max = _sequenceFromFiles->getLastFrame();
        }
    }
    
    
    //// From http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#SettingParams
//    Plugins are free to set parameters in limited set of circumstances, typically relating to user interaction. You can only set parameters in the following actions passed to the plug-in's main entry function...
//    
//    The Create Instance Action
//    The The Begin Instance Changed Action
//    The The Instance Changed Action
//    The The End Instance Changed Action
//    The The Sync Private Data Action
    if (canSetOriginalFrameRange) {
        _originalFrameRange->setValue(range.min, range.max);
    }
    return true;
}


void GenericReaderPlugin::timeDomainFromSequenceTimeDomain(OfxRangeD& range,bool mustSetFrameRange)
{
    ///the values held by GUI parameters
    int frameRangeFirst,frameRangeLast;
    int startingFrame;
    if (mustSetFrameRange) {
        frameRangeFirst = range.min;
        frameRangeLast = range.max;
        startingFrame = frameRangeFirst;
        _settingFrameRange = true;
        _firstFrame->setDisplayRange(range.min, range.max);
        _lastFrame->setDisplayRange(range.min, range.max);
        
        _firstFrame->setValue(range.min);
        _lastFrame->setValue(range.max);
        
        _originalFrameRange->setValue(range.min, range.max);
        _settingFrameRange = false;
    } else {
        ///these are the value held by the "First frame" and "Last frame" param
        _firstFrame->getValue(frameRangeFirst);
        _lastFrame->getValue(frameRangeLast);
        _startingFrame->getValue(startingFrame);
    }
    
    range.min = startingFrame;
    range.max = startingFrame + frameRangeLast - frameRangeFirst;
    
}

double GenericReaderPlugin::getSequenceTime(double t,bool canSetOriginalFrameRange)
{
    int timeOffset;
    _timeOffset->getValue(timeOffset);
    
    ///get the time sequence domain
    OfxRangeI sequenceTimeDomain;
    _firstFrame->getValue(sequenceTimeDomain.min);
    _lastFrame->getValue(sequenceTimeDomain.max);
    
    OfxRangeD originalTimeDomain;
    getSequenceTimeDomainInternal(originalTimeDomain,canSetOriginalFrameRange);
    
    ///the return value
    int sequenceTime =  t - timeOffset ;

    
    ///get the offset from the starting time of the sequence in case we bounce or loop
    int timeOffsetFromStart = t -  sequenceTimeDomain.min;
    
    ///if the time given is before the sequence
    if( sequenceTime < sequenceTimeDomain.min) {
        /////if we're before the first frame
        int beforeChoice;
        _beforeFirst->getValue(beforeChoice);
        switch (beforeChoice) {
            case 0: //hold
                sequenceTime = sequenceTimeDomain.min;
                break;
            case 1: //loop
                timeOffsetFromStart %= (int)(sequenceTimeDomain.max - sequenceTimeDomain.min + 1);
                sequenceTime = sequenceTimeDomain.max + timeOffsetFromStart;
                break;
            case 2: //bounce
            {
                int sequenceIntervalsCount = timeOffsetFromStart / (sequenceTimeDomain.max - sequenceTimeDomain.min);
                ///if the sequenceIntervalsCount is odd then do exactly like loop, otherwise do the load the opposite frame
                if (sequenceIntervalsCount % 2 == 0) {
                    timeOffsetFromStart %= (int)(sequenceTimeDomain.max - sequenceTimeDomain.min + 1);
                    sequenceTime = sequenceTimeDomain.min - timeOffsetFromStart;
                } else {
                    timeOffsetFromStart %= (int)(sequenceTimeDomain.max - sequenceTimeDomain.min + 1);
                    sequenceTime = sequenceTimeDomain.max + timeOffsetFromStart;
                }
            }
                break;
            case 3: //black
                break;
            case 4: //error
                setPersistentMessage(OFX::Message::eMessageError, "", "Out of frame range");
                throw std::invalid_argument("Out of frame range.");
                break;
            default:
                break;
        }
        clearPersistentMessage();
        assert(beforeChoice == 3 || (sequenceTime >= sequenceTimeDomain.min && sequenceTime <= sequenceTimeDomain.max));

    } else if( sequenceTime > sequenceTimeDomain.max) { ///the time given is after the sequence
                                             /////if we're after the last frame
        int afterChoice;
        _afterLast->getValue(afterChoice);
        
        switch (afterChoice) {
            case 0: //hold
                sequenceTime = sequenceTimeDomain.max;
                break;
            case 1: //loop
                timeOffsetFromStart %= (int)(sequenceTimeDomain.max - sequenceTimeDomain.min + 1);
                sequenceTime = sequenceTimeDomain.min + timeOffsetFromStart;
                break;
            case 2: //bounce
            {
                int sequenceIntervalsCount = timeOffsetFromStart / (sequenceTimeDomain.max - sequenceTimeDomain.min);
                ///if the sequenceIntervalsCount is odd then do exactly like loop, otherwise do the load the opposite frame
                if (sequenceIntervalsCount % 2 == 0) {
                    timeOffsetFromStart %= (int)(sequenceTimeDomain.max - sequenceTimeDomain.min + 1);
                    sequenceTime = sequenceTimeDomain.min + timeOffsetFromStart;
                } else {
                    timeOffsetFromStart %= (int)(sequenceTimeDomain.max - sequenceTimeDomain.min + 1);
                    sequenceTime = sequenceTimeDomain.max - timeOffsetFromStart;
                }
            }
                
                break;
            case 3: //black
                break;
            case 4: //error
                setPersistentMessage(OFX::Message::eMessageError, "", "Out of frame range");
                throw std::invalid_argument("Out of frame range.");
                break;
            default:
                break;
        }
        clearPersistentMessage();
        assert(afterChoice == 3 || (sequenceTime >= sequenceTimeDomain.min && sequenceTime <= sequenceTimeDomain.max));
    }
    
    return sequenceTime;
}

void GenericReaderPlugin::getFilenameAtSequenceTime(double sequenceTime, std::string &filename,bool proxyFiles)
{

    
    OfxRangeD sequenceTimeDomain;
    getSequenceTimeDomainInternal(sequenceTimeDomain,false);
    
    _fileParam->getValueAtTime(sequenceTime,filename);
    if (proxyFiles) {
        std::string proxyFileName;
        _proxyFileParam->getValueAtTime(sequenceTime, proxyFileName);
        if (!proxyFileName.empty() && !filename.empty()) {
            filename = proxyFileName;
        }
    }
    
    ///if the frame is missing, do smthing according to the missing frame param
    if (filename.empty()) {
        int missingChoice;
        _missingFrameParam->getValue(missingChoice);
        switch (missingChoice) {
            case 0: // Load nearest
            {
                int offset = -1;
                int maxOffset = MAX_SEARCH_RANGE;
                while (filename.empty() && offset <= maxOffset) {
                    _fileParam->getValueAtTime(sequenceTime + offset, filename);
                    if (offset < 0) {
                        offset = -offset;
                    } else {
                        ++offset;
                    }
                }
                if(filename.empty()){
                    setPersistentMessage(OFX::Message::eMessageError, "", "Nearest frame search went out of range");
                    // return a black image
                } else {
                    clearPersistentMessage();
                }
            }
                break;
            case 1: // Error
                /// For images sequences, we can safely say this is  a missing frame. For video-streams we do not know and the derived class
                // will have to handle the case itself.
                setPersistentMessage(OFX::Message::eMessageError, "", "Missing frame");
                break;
            case 2: // Black image
                /// For images sequences, we can safely say this is  a missing frame. For video-streams we do not know and the derived class
                // will have to handle the case itself.
                clearPersistentMessage();
                break;
        }

    }
    
    
    
}

void GenericReaderPlugin::getCurrentFileName(std::string& filename) {
    _fileParam->getValue(filename);
}

/* set up and run a copy processor */
static void setupAndCopy(OFX::PixelProcessorFilterBase & processor,
                         const OfxRectI &renderWindow,
                         const void *srcPixelData,
                         const OfxRectI& srcBounds,
                         OFX::PixelComponentEnum srcPixelComponents,
                         OFX::BitDepthEnum srcPixelDepth,
                         int srcRowBytes,
                         void *dstPixelData,
                         const OfxRectI& dstBounds,
                         OFX::PixelComponentEnum dstPixelComponents,
                         OFX::BitDepthEnum dstPixelDepth,
                         int dstRowBytes)
{
    assert(srcPixelData && dstPixelData);

    // make sure bit depths are sane
    if(srcPixelDepth != dstPixelDepth || srcPixelComponents != dstPixelComponents) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }

    // set the images
    processor.setDstImg(dstPixelData, dstBounds, dstPixelComponents, dstPixelDepth, dstRowBytes);
    processor.setSrcImg(srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes);

    // set the render window
    processor.setRenderWindow(renderWindow);

    // Call the base class process member, this will call the derived templated process code
    processor.process();
}

void GenericReaderPlugin::copyPixelData(const OfxRectI& renderWindow,
                                        const void *srcPixelData,
                                        const OfxRectI& srcBounds,
                                        OFX::PixelComponentEnum srcPixelComponents,
                                        OFX::BitDepthEnum srcPixelDepth,
                                        int srcRowBytes,
                                        void *dstPixelData,
                                        const OfxRectI& dstBounds,
                                        OFX::PixelComponentEnum dstPixelComponents,
                                        OFX::BitDepthEnum dstBitDepth,
                                        int dstRowBytes)
{
    assert(srcPixelData && dstPixelData);

    // do the rendering
    if (dstBitDepth != OFX::eBitDepthFloat || (dstPixelComponents != OFX::ePixelComponentRGBA && dstPixelComponents != OFX::ePixelComponentRGB && dstPixelComponents != OFX::ePixelComponentAlpha)) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }
    if(dstPixelComponents == OFX::ePixelComponentRGBA) {
        PixelCopier<float, 4> fred(*this);
        setupAndCopy(fred, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
    } else if(dstPixelComponents == OFX::ePixelComponentRGB) {
        PixelCopier<float, 3> fred(*this);
        setupAndCopy(fred, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
    }  else if(dstPixelComponents == OFX::ePixelComponentAlpha) {
        PixelCopier<float, 1> fred(*this);
        setupAndCopy(fred, renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstBitDepth, dstRowBytes);
    } // switch
}


/* set up and run a copy processor */
static void setupAndScale(OFX::PixelScalerProcessorFilterBase & processor,
                         const void *srcPixelData,
                         const OfxRectI& srcBounds,
                         OFX::PixelComponentEnum srcPixelComponents,
                         OFX::BitDepthEnum srcPixelDepth,
                         int srcRowBytes,
                         void *dstPixelData,
                         const OfxRectI& dstBounds,
                         OFX::PixelComponentEnum dstPixelComponents,
                         OFX::BitDepthEnum dstPixelDepth,
                         int dstRowBytes)
{
    assert(srcPixelData && dstPixelData);
    
    // make sure bit depths are sane
    if(srcPixelDepth != dstPixelDepth || srcPixelComponents != dstPixelComponents) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }
    
    // set the images
    processor.setDstImg(dstPixelData, dstBounds, dstPixelComponents, dstPixelDepth, dstRowBytes);
    processor.setSrcImg(srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes);
    
    OfxPointD scale;
    scale.x = (double)(dstBounds.x2 - dstBounds.x1) / (double)(srcBounds.x2 - srcBounds.x1);
    scale.y = (double)(dstBounds.y2 - dstBounds.y1) / (double)(srcBounds.y2 - srcBounds.y1);
    processor.setScale(scale);
    
    // set the render window
    processor.setRenderWindow(dstBounds);
    
    // Call the base class process member, this will call the derived templated process code
    processor.process();
}

void GenericReaderPlugin::scalePixelData(const void* srcPixelData,
                    OFX::PixelComponentEnum srcPixelComponents,
                    OFX::BitDepthEnum srcPixelDepth,
                    const OfxRectI& srcBounds,
                    int srcRowBytes,
                    void* dstPixelData,
                    OFX::PixelComponentEnum dstPixelComponents,
                    OFX::BitDepthEnum dstPixelDepth,
                    const OfxRectI& dstBounds,
                    int dstRowBytes)
{
    
    assert(srcPixelData && dstPixelData);
    
    // do the rendering
    if (dstPixelDepth != OFX::eBitDepthFloat || (dstPixelComponents != OFX::ePixelComponentRGBA && dstPixelComponents != OFX::ePixelComponentRGB && dstPixelComponents != OFX::ePixelComponentAlpha)) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }
    if(dstPixelComponents == OFX::ePixelComponentRGBA) {
        PixelScaler<float, 4> fred(*this);
        setupAndScale(fred, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstPixelDepth, dstRowBytes);
    } else if(dstPixelComponents == OFX::ePixelComponentRGB) {
        PixelScaler<float, 3> fred(*this);
        setupAndScale(fred, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstPixelDepth, dstRowBytes);
    }  else if(dstPixelComponents == OFX::ePixelComponentAlpha) {
        PixelScaler<float, 1> fred(*this);
        setupAndScale(fred, srcPixelData, srcBounds, srcPixelComponents, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstPixelDepth, dstRowBytes);
    } // switch


}

bool GenericReaderPlugin::getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod){
    
    double sequenceTime;
    try {
        sequenceTime =  getSequenceTime(args.time,false);
    } catch (const std::exception& e) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    std::string filename;
    ///Retrieve the original filename, we don't care about the proxy images here because the rod is
    ///in canonical coordinates (it doesn't care about the render scale)
    getFilenameAtSequenceTime(sequenceTime, filename,false);
    
    if (filename.empty()) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    
    getFrameRegionOfDefinition(filename, sequenceTime, rod);
    return true;
}

void GenericReaderPlugin::render(const OFX::RenderArguments &args)
{
    if (!_outputClip) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    
    ///The image will have the appropriate size since we support the render scale (multi-resolution)
    std::auto_ptr<OFX::Image> dstImg(_outputClip->fetchImage(args.time));
    if (!dstImg.get()) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    double sequenceTime;
    try {
        sequenceTime =  getSequenceTime(args.time,false);
    } catch (const std::exception& e) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    
    bool useProxy = false;
    if (args.renderScale.x != 1. || args.renderScale.y != 1) {
        //    useProxy = true;
    }

    
    std::string filename;
    getFilenameAtSequenceTime(sequenceTime, filename,false);
    
    std::string proxyFile;
    if (useProxy) {
        getFilenameAtSequenceTime(sequenceTime, proxyFile, true);
        assert(!proxyFile.empty());
    }
    bool validProxy = proxyFile != filename;
    

    void* dstPixelData = NULL;
    OfxRectI bounds;
    OFX::PixelComponentEnum pixelComponents;
    OFX::BitDepthEnum bitDepth;
    int dstRowBytes;
    getImageData(dstImg.get(), &dstPixelData, &bounds, &pixelComponents, &bitDepth, &dstRowBytes);
    if (bitDepth != OFX::eBitDepthFloat) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }
    float* dstPixelDataF = (float*)dstPixelData;

    if (pixelComponents != OFX::ePixelComponentRGBA && pixelComponents != OFX::ePixelComponentRGB && pixelComponents != OFX::ePixelComponentAlpha) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }

    // are we in the image bounds
    if(args.renderWindow.x1 < bounds.x1 || args.renderWindow.x1 >= bounds.x2 || args.renderWindow.y1 < bounds.y1 || args.renderWindow.y1 >= bounds.y2 ||
       args.renderWindow.x2 <= bounds.x1 || args.renderWindow.x2 > bounds.x2 || args.renderWindow.y2 <= bounds.y1 || args.renderWindow.y2 > bounds.y2) {
        OFX::throwSuiteStatusException(kOfxStatErrValue);
        //throw std::runtime_error("render window outside of image bounds");
    }

    if (filename.empty()) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    
    ///The args.renderWindow is already in pixels coordinate (render scale is already taken into account ).
    ///If the filename IS NOT a proxy file we have to make sure the renderWindow is
    ///upscaled to a scale of (1,1). On the other hand if the filename IS a proxy we have to determine the actual RoD
    ///of the proxy file and adjust the scale so it fits the given scale.
    // allocate
    OfxRectI renderWindowToUse = args.renderWindow;
    OfxPointD backScale;
    if (useProxy) {
        
        if (!validProxy) {
            ///the user didn't provide a proxy file, just decode the full image
            ///upscale to a render scale of 1.
            renderWindowToUse = scaled(args.renderWindow, 1. / args.renderScale.x, 1. / args.renderScale.y);
            backScale = args.renderScale;
        } else {
            ///The user provided a proxyFile, scale it according to the render scale if needed
            OfxRectD proxyFileRoD;
            getFrameRegionOfDefinition(proxyFile, args.time, proxyFileRoD);
            OfxRectD originalFileRoD;
            getFrameRegionOfDefinition(filename, args.time, originalFileRoD);
            
            OfxPointD proxyActualScale;
            proxyActualScale.x =  (proxyFileRoD.x2 - proxyFileRoD.x1) / (originalFileRoD.x2 - originalFileRoD.x1);
            proxyActualScale.y =  (proxyFileRoD.y2 - proxyFileRoD.y1) / (originalFileRoD.y2 - originalFileRoD.y1);
            
            ///This is by how much we need to scale the proxy file in order to match the requested render scale
            OfxPointD scaleAdjust;
            scaleAdjust.x = (args.renderScale.x / proxyActualScale.x);
            scaleAdjust.y = (args.renderScale.y / proxyActualScale.y);
            renderWindowToUse = scaled(args.renderWindow, 1. / scaleAdjust.x,1. /  scaleAdjust.y);
            backScale.x = scaleAdjust.x;
            backScale.y = scaleAdjust.y;
        }
    }

    
    // The following (commented out) code is not fully-safe, because the same instance may be have
    // two threads running on the same area of the same frame, and the apply()
    // calls both read and write dstImg.
    // This results in colorspace conversion being applied several times.
    //
    //if (!filename.empty()) {
    //    decode(filename, sequenceTime, args.renderWindow, dstImg.get());
    //}
    /////do the color-space conversion
    //_ocio->apply(args.renderWindow, dstImg.get());

    // Good solution: read into a temporary image, apply colorspace conversion, then copy.

    if (_ocio->isIdentity()) {
        // no colorspace conversion, just read file
        
        if (!useProxy) {
            decode(filename, sequenceTime, args.renderWindow, dstPixelDataF, bounds, pixelComponents, dstRowBytes);
        } else {
            int pixelBytes = getPixelBytes(pixelComponents, bitDepth);
            int tmpRowBytes = (renderWindowToUse.x2-renderWindowToUse.x1) * pixelBytes;
            size_t memSize = (renderWindowToUse.y2-renderWindowToUse.y1) * tmpRowBytes;
            OFX::ImageMemory mem(memSize,this);
            float *tmpPixelData = (float*)mem.lock();
            
            // read file
            if (!validProxy) {
                decode(filename, sequenceTime, renderWindowToUse, tmpPixelData, renderWindowToUse, pixelComponents, tmpRowBytes);
            } else {
                decode(proxyFile, sequenceTime, renderWindowToUse, tmpPixelData, renderWindowToUse, pixelComponents, tmpRowBytes);
            }
            
            if (backScale.x != 1. || backScale.y != 1.) {
                /// adjust the scale to match the given output image
                scalePixelData(tmpPixelData, pixelComponents, bitDepth, renderWindowToUse, tmpRowBytes, dstPixelData,
                               pixelComponents, bitDepth, args.renderWindow, dstRowBytes);
            } else {
                copyPixelData(args.renderWindow, tmpPixelData, args.renderWindow, pixelComponents, bitDepth, tmpRowBytes, dstPixelData, bounds, pixelComponents, bitDepth, dstRowBytes);
            }
            
            mem.unlock();
        }
    } else {
        
        int pixelBytes = getPixelBytes(pixelComponents, bitDepth);
        int tmpRowBytes = (renderWindowToUse.x2-renderWindowToUse.x1) * pixelBytes;
        size_t memSize = (renderWindowToUse.y2-renderWindowToUse.y1) * tmpRowBytes;
        OFX::ImageMemory mem(memSize,this);
        float *tmpPixelData = (float*)mem.lock();

        // read file
        if (!validProxy || !useProxy) {
            decode(filename, sequenceTime, renderWindowToUse, tmpPixelData, renderWindowToUse, pixelComponents, tmpRowBytes);
        } else {
            decode(proxyFile, sequenceTime, renderWindowToUse, tmpPixelData, renderWindowToUse, pixelComponents, tmpRowBytes);
        }

        ///do the color-space conversion
        _ocio->apply(renderWindowToUse, tmpPixelData, renderWindowToUse, pixelComponents, tmpRowBytes);

        if (useProxy) {
            if (backScale.x != 1. || backScale.y != 1.) {
            /// adjust the scale to match the given output image
                scalePixelData(tmpPixelData, pixelComponents, bitDepth, renderWindowToUse, tmpRowBytes, dstPixelData,
                           pixelComponents, bitDepth, args.renderWindow, dstRowBytes);
            } else {
                copyPixelData(args.renderWindow, tmpPixelData, args.renderWindow, pixelComponents, bitDepth, tmpRowBytes, dstPixelData, bounds, pixelComponents, bitDepth, dstRowBytes);
            }
        } else{
        // copy
            copyPixelData(args.renderWindow, tmpPixelData, args.renderWindow, pixelComponents, bitDepth, tmpRowBytes, dstPixelData, bounds, pixelComponents, bitDepth, dstRowBytes);
        }
        mem.unlock();
    }
}

void GenericReaderPlugin::inputFileChanged() {
    std::string filename;
    _fileParam->getValue(filename);
    
    try {
        delete _sequenceFromFiles;
        _sequenceFromFiles = new SequenceParsing::SequenceFromFiles();
        SequenceParsing::SequenceFromFiles::getSequenceOutOfFile(filename, _sequenceFromFiles);
    } catch(const std::exception& e) {
        setPersistentMessage(OFX::Message::eMessageError, "", e.what());
        return;
    }
    clearPersistentMessage();
    //reset the original range param
    _originalFrameRange->setValue(INT_MIN, INT_MAX);
    
    
    ///let the derive class a chance to initialize any data structure it may need
    onInputFileChanged(filename);
    
    ///we don't pass the _frameRange range as we don't want to store the time domain too
    OfxRangeD tmp;
    getSequenceTimeDomainInternal(tmp,true);
    timeDomainFromSequenceTimeDomain(tmp, true);
    _startingFrame->setValue(tmp.min);
 
}

void GenericReaderPlugin::changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName) {
    if(paramName == kReaderFileParamName) {
        if (args.reason != OFX::eChangeTime) {
            inputFileChanged();
        }
    } else if( paramName == kReaderFirstFrameParamName && !_settingFrameRange) {
        int first;
        int last;
        _firstFrame->getValue(first);
        _lastFrame->getValue(last);
        _lastFrame->setDisplayRange(first, last);

        int offset;
        _timeOffset->getValue(offset);
         _settingFrameRange = true,
        _startingFrame->setValue(first + offset);
        _settingFrameRange = false;
    } else if( paramName == kReaderLastFrameParamName && !_settingFrameRange) {
        int first;
        int last;
        _firstFrame->getValue(first);
        _lastFrame->getValue(last);
        _firstFrame->setDisplayRange(first, last);
    } else if( paramName == kReaderFrameModeParamName ) {
        int mode;
        _frameMode->getValue(mode);
        switch (mode) {
            case 0: //starting frame
                _startingFrame->setIsSecret(false);
                _timeOffset->setIsSecret(true);
                break;
            case 1: //time offset
                _startingFrame->setIsSecret(true);
                _timeOffset->setIsSecret(false);
                break;
            default:
                //no such case
                assert(false);
                break;
        }
    } else if( paramName == kReaderStartingFrameParamName && !_settingFrameRange) {
        
        ///recompute the timedomain
        OfxRangeD sequenceTimeDomain;
        getSequenceTimeDomainInternal(sequenceTimeDomain,true);
        
        //also update the time offset
        int startingFrame;
        _startingFrame->getValue(startingFrame);
        
        int firstFrame;
        _firstFrame->getValue(firstFrame);
        
        ///prevent recursive calls of setValue(...)
        _settingFrameRange = true;
        _timeOffset->setValue(startingFrame - firstFrame);
        _settingFrameRange = false;
        
    } else if( paramName == kReaderTimeOffsetParamName && !_settingFrameRange) {
        //also update the starting frame
        int offset;
        _timeOffset->getValue(offset);
        int first;
        _firstFrame->getValue(first);
        
        ///prevent recursive calls of setValue(...)
        _settingFrameRange = true;
        _startingFrame->setValue(offset + first);
        _settingFrameRange = false;
    } else {
        _ocio->changedParam(args, paramName);
    }
}

void GenericReaderPlugin::purgeCaches() {
    clearAnyCache();
    _ocio->purgeCaches();
}

using namespace OFX;

void GenericReaderDescribe(OFX::ImageEffectDescriptor &desc, bool supportsTiles)
{
    desc.setPluginGrouping("Image/Readers");
    
#ifdef OFX_EXTENSIONS_TUTTLE
    desc.addSupportedContext(OFX::eContextReader);
#endif
    desc.addSupportedContext(OFX::eContextGenerator);
    desc.addSupportedContext(OFX::eContextGeneral);
    
    // add supported pixel depths
    //desc.addSupportedBitDepth( eBitDepthUByte );
    //desc.addSupportedBitDepth( eBitDepthUShort );
    desc.addSupportedBitDepth(eBitDepthFloat);
    
    // set a few flags
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    
    ///We do support multi-resolution via the proxy mode
    desc.setSupportsMultiResolution(false);
    
    
    desc.setSupportsTiles(supportsTiles);
    desc.setTemporalClipAccess(false); // say we will be doing random time access on clips
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);
}

OFX::PageParamDescriptor * GenericReaderDescribeInContextBegin(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum /*context*/, bool isVideoStreamPlugin, bool supportsRGBA, bool supportsRGB, bool supportsAlpha, bool supportsTiles)
{
    // make some pages and to things in
    PageParamDescriptor *page = desc.definePageParam("Controls");

    // create the optional source clip
    ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    if (supportsRGBA) {
        srcClip->addSupportedComponent(ePixelComponentRGBA);
    }
    if (supportsRGB) {
        srcClip->addSupportedComponent(ePixelComponentRGB);
    }
    if (supportsAlpha) {
        srcClip->addSupportedComponent(ePixelComponentAlpha);
    }
    srcClip->setSupportsTiles(supportsTiles);
    srcClip->setOptional(true);
    
    // create the mandated output clip
    ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    if (supportsRGBA) {
        dstClip->addSupportedComponent(ePixelComponentRGBA);
    }
    if (supportsRGB) {
        dstClip->addSupportedComponent(ePixelComponentRGB);
    }
    if (supportsAlpha) {
        dstClip->addSupportedComponent(ePixelComponentAlpha);
    }
    dstClip->setSupportsTiles(supportsTiles);
    

    //////////Input file
    OFX::StringParamDescriptor* fileParam = desc.defineStringParam(kReaderFileParamName);
    fileParam->setLabels("File", "File", "File");
    fileParam->setStringType(OFX::eStringTypeFilePath);
    fileParam->setHint("The input image sequence/video stream file(s).");
    fileParam->setAnimates(!isVideoStreamPlugin);
    // in the Reader context, the script name must be "filename", @see kOfxImageEffectContextReader
    fileParam->setScriptName(kReaderFileParamName);
    desc.addClipPreferencesSlaveParam(*fileParam);
    page->addChild(*fileParam);
    
    //////////First-frame
    OFX::IntParamDescriptor* firstFrameParam = desc.defineIntParam(kReaderFirstFrameParamName);
    firstFrameParam->setLabels("First frame", "First frame", "First frame");
    firstFrameParam->setHint("The first frame this sequence/video should start at. This cannot be lesser "
                             " than the first frame of the sequence and cannot be greater than the last"
                             " frame of the sequence.");
    firstFrameParam->setDefault(0);
    firstFrameParam->setAnimates(false);
    firstFrameParam->setLayoutHint(OFX::eLayoutHintNoNewLine);
    page->addChild(*firstFrameParam);
    
    ///////////Before first
    OFX::ChoiceParamDescriptor* beforeFirstParam = desc.defineChoiceParam(kReaderBeforeParamName);
    beforeFirstParam->setLabels("Before", "Before", "Before");
    beforeFirstParam->setHint("What to do before the first frame of the sequence.");
    beforeFirstParam->appendOption("hold","While before the sequence, load the first frame.");
    beforeFirstParam->appendOption("loop","Repeat the sequence before the first frame");
    beforeFirstParam->appendOption("bounce","Repeat the sequence in reverse before the first frame");
    beforeFirstParam->appendOption("black","Render a black image");
    beforeFirstParam->appendOption("error","Report an error");
    beforeFirstParam->setAnimates(false);
    beforeFirstParam->setDefault(0);
    page->addChild(*beforeFirstParam);
    
    //////////Last-frame
    OFX::IntParamDescriptor* lastFrameParam = desc.defineIntParam(kReaderLastFrameParamName);
    lastFrameParam->setLabels("Last frame", "Last frame", "Last frame");
    lastFrameParam->setHint("The frame this sequence/video should end at. This cannot be lesser "
                            " than the first frame of the sequence and cannot be greater than the last"
                            " frame of the sequence.");
    lastFrameParam->setDefault(0);
    lastFrameParam->setAnimates(false);
    lastFrameParam->setLayoutHint(OFX::eLayoutHintNoNewLine);
    page->addChild(*lastFrameParam);
    
    ///////////After first
    OFX::ChoiceParamDescriptor* afterLastParam = desc.defineChoiceParam(kReaderAfterParamName);
    afterLastParam->setLabels("After", "After", "After");
    afterLastParam->setHint("What to do after the last frame of the sequence.");
    afterLastParam->appendOption("hold","While after the sequence, load the last frame.");
    afterLastParam->appendOption("loop","Repeat the sequence after the last frame");
    afterLastParam->appendOption("bounce","Repeat the sequence in reverse after the last frame");
    afterLastParam->appendOption("black","Render a black image");
    afterLastParam->appendOption("error","Report an error");
    afterLastParam->setAnimates(false);
    afterLastParam->setDefault(0);
    page->addChild(*afterLastParam);
    
    ///////////Missing frame choice
    OFX::ChoiceParamDescriptor* missingFrameParam = desc.defineChoiceParam(kReaderMissingFrameParamName);
    missingFrameParam->setLabels("On Missing Frame", "On Missing Frame", "On Missing Frame");
    missingFrameParam->setHint("What to do when a frame is missing from the sequence/stream.");
    missingFrameParam->appendOption("Load nearest","Tries to load the nearest frame in the sequence/stream if any.");
    missingFrameParam->appendOption("Error","An error is reported.");
    missingFrameParam->appendOption("Black image","A black image is rendered.");
    missingFrameParam->setAnimates(false);
    missingFrameParam->setDefault(0); //< default to nearest frame.
    page->addChild(*missingFrameParam);
    
    
    ///////////Frame-mode
    OFX::ChoiceParamDescriptor* frameModeParam = desc.defineChoiceParam(kReaderFrameModeParamName);
    frameModeParam->appendOption("Starting frame");
    frameModeParam->appendOption("Time offset");
    frameModeParam->setAnimates(false);
    frameModeParam->setDefault(0);
    frameModeParam->setLayoutHint(OFX::eLayoutHintNoNewLine);
    page->addChild(*frameModeParam);
    
    ///////////Starting frame
    OFX::IntParamDescriptor* startingFrameParam = desc.defineIntParam(kReaderStartingFrameParamName);
    startingFrameParam->setLabels("Starting time", "Starting time", "Starting time");
    startingFrameParam->setHint("At what time (on the timeline) should this sequence/video start.");
    startingFrameParam->setDefault(0);
    startingFrameParam->setAnimates(false);
    startingFrameParam->setLayoutHint(OFX::eLayoutHintNoNewLine);
    page->addChild(*startingFrameParam);
    
    ///////////Time offset
    OFX::IntParamDescriptor* timeOffsetParam = desc.defineIntParam(kReaderTimeOffsetParamName);
    timeOffsetParam->setLabels("Time offset", "Time offset", "Time offset");
    timeOffsetParam->setHint("Offset applied to the sequence in frames.");
    timeOffsetParam->setDefault(0);
    timeOffsetParam->setAnimates(false);
    timeOffsetParam->setIsSecret(true);
    page->addChild(*timeOffsetParam);
    
    ///////////Original frame range
    OFX::Int2DParamDescriptor* originalFrameRangeParam = desc.defineInt2DParam(kReaderOriginalFrameRangeParamName);
    originalFrameRangeParam->setLabels("Original range", "Original range", "Original range");
    originalFrameRangeParam->setDefault(INT_MIN, INT_MAX);
    originalFrameRangeParam->setAnimates(false);
    originalFrameRangeParam->setIsSecret(true);
    originalFrameRangeParam->setIsPersistant(false);
    page->addChild(*originalFrameRangeParam);
    
    
    //////////Input proxy file
    OFX::StringParamDescriptor* proxyFileParam = desc.defineStringParam(kReaderProxyFileParamName);
    proxyFileParam->setLabels("Proxy file", "Proxy file", "Proxy file");
    proxyFileParam->setStringType(OFX::eStringTypeFilePath);
    proxyFileParam->setHint("Filename of the proxy images. They will be used instead of the images read from the File parameter "
                       "when the proxy mode (downscaling of the images) is activated.");
    proxyFileParam->setAnimates(!isVideoStreamPlugin);
    // in the Reader context, the script name must be "filename", @see kOfxImageEffectContextReader
    proxyFileParam->setScriptName(kReaderProxyFileParamName);
    desc.addClipPreferencesSlaveParam(*proxyFileParam);
    page->addChild(*proxyFileParam);
    
    return page;
}


void GenericReaderDescribeInContextEnd(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context, OFX::PageParamDescriptor* page, const char* inputSpaceNameDefault, const char* outputSpaceNameDefault) {
    // insert OCIO parameters
    GenericOCIO::describeInContext(desc, context, page, inputSpaceNameDefault, outputSpaceNameDefault);
}


