#pragma once
// Call only after xrBeginFrame. An unavailable eye must discard the whole pair.
template<class AcquireWait,class Render,class Release,class End>
bool PresentStereoFrame(bool visible,AcquireWait acquireWait,Render render,Release release,End end) {
    try {
        if(visible) for(unsigned eye=0;eye<2;++eye) {
            acquireWait(eye);
            bool rendered=false;
            try { rendered=render(eye); }
            catch(...) { release(eye);throw; }
            release(eye);
            if(!rendered) { visible=false;break; }
        }
    } catch(...) { end(false);throw; }
    end(visible);return visible;
}
