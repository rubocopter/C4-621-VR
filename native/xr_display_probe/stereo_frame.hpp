#pragma once
// Call only after xrBeginFrame. Validate the source after both copies/releases;
// success from an earlier eye does not establish availability at submission.
template<class AcquireWait,class Render,class Release,class ValidateSource,class End>
bool PresentStereoFrame(bool visible,AcquireWait acquireWait,Render render,Release release,ValidateSource validateSource,End end) {
    try {
        if(visible) for(unsigned eye=0;eye<2;++eye) {
            acquireWait(eye);
            bool rendered=false;
            try { rendered=render(eye); }
            catch(...) { release(eye);throw; }
            release(eye);
            if(!rendered) { visible=false;break; }
        }
        if(visible) visible=validateSource();
    } catch(...) { end(false);throw; }
    end(visible);return visible;
}
