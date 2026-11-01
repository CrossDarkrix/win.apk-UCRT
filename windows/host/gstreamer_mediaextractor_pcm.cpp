// GStreamer-backed NativeMedia audio extractor for Windows.
// Exposes decoded S16LE PCM blocks through MediaExtractor and uses the existing
// MediaCodec audio JNI surface as a strict PCM pass-through.
#include "host_natives.h"
#include <jni.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <vector>
#ifdef WINAPK_HAVE_GSTREAMER
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#endif
namespace {
#ifdef WINAPK_HAVE_GSTREAMER
struct Demux { std::mutex lock; GstElement* pipeline=nullptr; GstAppSink* sink=nullptr; GstSample* current=nullptr; int rate=0,channels=0; int64_t duration_us=-1,pts_us=-1,mp3_duration_us=-1; bool eos=false; std::string error; };
struct Pass { int rate=0, channels=0; };
struct Mp3FrameInfo { int sample_rate=0; int samples=0; int frame_bytes=0; };
bool mp3_frame_info(const unsigned char*h,Mp3FrameInfo*out){if(!h||!out||h[0]!=0xff||(h[1]&0xe0)!=0xe0)return false;int version=(h[1]>>3)&3,layer=(h[1]>>1)&3,bitrate_index=(h[2]>>4)&15,sample_index=(h[2]>>2)&3,padding=(h[2]>>1)&1;if(version==1||layer!=1||bitrate_index==0||bitrate_index==15||sample_index==3)return false;static const int sr[3]={44100,48000,32000};static const int br1[16]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};static const int br2[16]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};int rate=sr[sample_index];if(version==2)rate/=2;else if(version==0)rate/=4;int kbps=version==3?br1[bitrate_index]:br2[bitrate_index];int samples=version==3?1152:576;int bytes=((version==3?144000:72000)*kbps)/rate+padding;if(rate<=0||bytes<24)return false;out->sample_rate=rate;out->samples=samples;out->frame_bytes=bytes;return true;}
int64_t mp3_duration_fallback(const char*path){if(!path||!*path)return -1;std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)return -1;std::streamoff end=f.tellg();if(end<4)return -1;uintmax_t file_size=(uintmax_t)end;if(file_size>(uintmax_t)std::numeric_limits<size_t>::max()||file_size>(uintmax_t)std::numeric_limits<std::streamsize>::max())return -1;size_t size=(size_t)file_size;std::vector<unsigned char>data(size);f.seekg(0,std::ios::beg);if(!f.read((char*)data.data(),(std::streamsize)size))return -1;size_t pos=0,limit=data.size();if(limit>=10&&std::memcmp(data.data(),"ID3",3)==0){size_t tag=((size_t)(data[6]&0x7f)<<21)|((size_t)(data[7]&0x7f)<<14)|((size_t)(data[8]&0x7f)<<7)|(size_t)(data[9]&0x7f);size_t skip=10+tag+((data[5]&0x10)?10:0);if(skip<limit)pos=skip;}if(limit>=128&&std::memcmp(data.data()+limit-128,"TAG",3)==0)limit-=128;int64_t duration=0;uint64_t frames=0;while(pos+4<=limit){Mp3FrameInfo fi;if(!mp3_frame_info(data.data()+pos,&fi)||pos+(size_t)fi.frame_bytes>limit){++pos;continue;}size_t next=pos+(size_t)fi.frame_bytes;if(next+4<=limit){Mp3FrameInfo ni;if(!mp3_frame_info(data.data()+next,&ni)){++pos;continue;}}duration+=(int64_t)fi.samples*1000000LL/fi.sample_rate;++frames;pos=next;}return frames>=2&&duration>0?duration:-1;}
std::once_flag once;
void init(){std::call_once(once,[]{gst_init(nullptr,nullptr);});}
void clear(Demux*d){if(d->current)gst_sample_unref(d->current);d->current=nullptr;d->pts_us=-1;}
bool check_bus(Demux*d){GstBus*b=gst_element_get_bus(d->pipeline);GstMessage*m=gst_bus_pop_filtered(b,(GstMessageType)(GST_MESSAGE_ERROR|GST_MESSAGE_EOS));gst_object_unref(b);if(!m)return false;if(GST_MESSAGE_TYPE(m)==GST_MESSAGE_ERROR){GError*e=nullptr;gchar*dbg=nullptr;gst_message_parse_error(m,&e,&dbg);d->error=e?e->message:"GStreamer error";if(e)g_error_free(e);g_free(dbg);}else d->eos=true;gst_message_unref(m);return true;}
bool pull(Demux*d){clear(d);while(!d->eos&&d->error.empty()){GstSample*s=gst_app_sink_try_pull_sample(d->sink,100*GST_MSECOND);if(s){d->current=s;GstCaps*c=gst_sample_get_caps(s);if(c&&gst_caps_get_size(c)){const GstStructure*st=gst_caps_get_structure(c,0);gst_structure_get_int(st,"rate",&d->rate);gst_structure_get_int(st,"channels",&d->channels);}GstBuffer*b=gst_sample_get_buffer(s);if(b&&GST_BUFFER_PTS_IS_VALID(b))d->pts_us=(int64_t)(GST_BUFFER_PTS(b)/GST_USECOND);return true;}check_bus(d);if(gst_app_sink_is_eos(d->sink))d->eos=true;}return false;}
Demux*open_file(const char*path){if(!path||!*path)return nullptr;init();GError*e=nullptr;const char*desc="filesrc name=source ! decodebin ! audioconvert ! audioresample ! audio/x-raw,format=S16LE,layout=interleaved ! appsink name=pcm sync=false max-buffers=8 drop=false";GstElement*p=gst_parse_launch(desc,&e);if(!p){if(e)g_error_free(e);return nullptr;}GstElement*src=gst_bin_get_by_name(GST_BIN(p),"source");if(!src){gst_object_unref(p);if(e)g_error_free(e);return nullptr;}g_object_set(G_OBJECT(src),"location",path,nullptr);gst_object_unref(src);GstElement*se=gst_bin_get_by_name(GST_BIN(p),"pcm");if(!se){gst_object_unref(p);if(e)g_error_free(e);return nullptr;}auto*d=new Demux;d->pipeline=p;d->sink=GST_APP_SINK(se);size_t plen=std::strlen(path);if(plen>=4&&_stricmp(path+plen-4,".mp3")==0)d->mp3_duration_us=mp3_duration_fallback(path);if(e)g_error_free(e);if(gst_element_set_state(p,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE||!pull(d)||d->rate<=0||d->channels<=0){gst_element_set_state(p,GST_STATE_NULL);clear(d);gst_object_unref(d->sink);gst_object_unref(p);delete d;return nullptr;}gint64 dur=-1;if(gst_element_query_duration(p,GST_FORMAT_TIME,&dur)&&dur>=0)d->duration_us=(int64_t)(dur/GST_USECOND);if(d->duration_us<=0&&d->mp3_duration_us>0)d->duration_us=d->mp3_duration_us;return d;}
void close_file(Demux*d){if(!d)return;gst_element_set_state(d->pipeline,GST_STATE_NULL);clear(d);gst_object_unref(d->sink);gst_object_unref(d->pipeline);delete d;}
jbyteArray bytes(JNIEnv*e,Demux*d){if(!d||!d->current)return nullptr;GstBuffer*b=gst_sample_get_buffer(d->current);GstMapInfo m{};if(!b||!gst_buffer_map(b,&m,GST_MAP_READ))return nullptr;if(m.size>(gsize)INT32_MAX){gst_buffer_unmap(b,&m);return nullptr;}jbyteArray o=e->NewByteArray((jsize)m.size);if(o&&m.size)e->SetByteArrayRegion(o,0,(jsize)m.size,(const jbyte*)m.data);gst_buffer_unmap(b,&m);return o;}
#endif
}
extern "C" {
JNIEXPORT jlong JNICALL Java_com_macapk_loader_NativeMedia_nativeDemuxOpen(JNIEnv*e,jclass,jstring path){
#ifdef WINAPK_HAVE_GSTREAMER
if(!path)return 0;const char*p=e->GetStringUTFChars(path,nullptr);if(!p)return 0;Demux*d=open_file(p);e->ReleaseStringUTFChars(path,p);return (jlong)d;
#else
(void)e;(void)path;return 0;
#endif
}
JNIEXPORT jint JNICALL Java_com_macapk_loader_NativeMedia_nativeDemuxTrackCount(JNIEnv*,jclass,jlong h){return h?1:0;}
JNIEXPORT jint JNICALL Java_com_macapk_loader_NativeMedia_nativeDemuxSampleRate(JNIEnv*,jclass,jlong h){
#ifdef WINAPK_HAVE_GSTREAMER
return h?((Demux*)h)->rate:0;
#else
return 0;
#endif
}
JNIEXPORT jint JNICALL Java_com_macapk_loader_NativeMedia_nativeDemuxChannels(JNIEnv*,jclass,jlong h){
#ifdef WINAPK_HAVE_GSTREAMER
return h?((Demux*)h)->channels:0;
#else
return 0;
#endif
}
JNIEXPORT jint JNICALL Java_com_macapk_loader_NativeMedia_nativeDemuxFormatId(JNIEnv*,jclass,jlong h){return h?1:0;}
JNIEXPORT jlong JNICALL Java_com_macapk_loader_NativeMedia_nativeDemuxDurationUs(JNIEnv*,jclass,jlong h){
#ifdef WINAPK_HAVE_GSTREAMER
auto*d=(Demux*)h;if(!d)return -1;std::lock_guard<std::mutex>g(d->lock);gint64 dur=-1;if(gst_element_query_duration(d->pipeline,GST_FORMAT_TIME,&dur)&&dur>0)d->duration_us=(int64_t)(dur/GST_USECOND);if(d->duration_us<=0&&d->mp3_duration_us>0)d->duration_us=d->mp3_duration_us;return d->duration_us;
#else
return -1;
#endif
}
JNIEXPORT jbyteArray JNICALL Java_com_macapk_loader_NativeMedia_nativeDemuxCsd0(JNIEnv*,jclass,jlong){return nullptr;}
JNIEXPORT jbyteArray JNICALL Java_com_macapk_loader_NativeMedia_nativeDemuxCurrent(JNIEnv*e,jclass,jlong h){
#ifdef WINAPK_HAVE_GSTREAMER
auto*d=(Demux*)h;if(!d)return nullptr;std::lock_guard<std::mutex>g(d->lock);return bytes(e,d);
#else
(void)e;(void)h;return nullptr;
#endif
}
JNIEXPORT jlong JNICALL Java_com_macapk_loader_NativeMedia_nativeDemuxSampleTime(JNIEnv*,jclass,jlong h){
#ifdef WINAPK_HAVE_GSTREAMER
return h?((Demux*)h)->pts_us:-1;
#else
return -1;
#endif
}
JNIEXPORT jint JNICALL Java_com_macapk_loader_NativeMedia_nativeDemuxAdvance(JNIEnv*,jclass,jlong h){
#ifdef WINAPK_HAVE_GSTREAMER
auto*d=(Demux*)h;if(!d)return 0;std::lock_guard<std::mutex>g(d->lock);return pull(d)?1:0;
#else
return 0;
#endif
}
JNIEXPORT void JNICALL Java_com_macapk_loader_NativeMedia_nativeDemuxSeek(JNIEnv*,jclass,jlong h,jlong us){
#ifdef WINAPK_HAVE_GSTREAMER
auto*d=(Demux*)h;if(!d)return;std::lock_guard<std::mutex>g(d->lock);if(us<0)us=0;gint64 dur=-1;if(gst_element_query_duration(d->pipeline,GST_FORMAT_TIME,&dur)&&dur>0)d->duration_us=(int64_t)(dur/GST_USECOND);if(d->duration_us<=0&&d->mp3_duration_us>0)d->duration_us=d->mp3_duration_us;if(d->duration_us>0&&us>=d->duration_us)us=d->duration_us>1000?d->duration_us-1000:0;clear(d);d->eos=false;d->error.clear();if(!gst_element_seek_simple(d->pipeline,GST_FORMAT_TIME,(GstSeekFlags)(GST_SEEK_FLAG_FLUSH|GST_SEEK_FLAG_ACCURATE),(gint64)us*GST_USECOND))return;GstState state=GST_STATE_NULL,pending=GST_STATE_VOID_PENDING;GstStateChangeReturn sr=gst_element_get_state(d->pipeline,&state,&pending,5*GST_SECOND);if(sr==GST_STATE_CHANGE_FAILURE)return;d->eos=false;d->error.clear();pull(d);
#else
(void)h;(void)us;
#endif
}
JNIEXPORT void JNICALL Java_com_macapk_loader_NativeMedia_nativeDemuxClose(JNIEnv*,jclass,jlong h){
#ifdef WINAPK_HAVE_GSTREAMER
close_file((Demux*)h);
#else
(void)h;
#endif
}
JNIEXPORT jlong JNICALL Java_com_macapk_loader_NativeMedia_nativeDecOpen(JNIEnv*,jclass,jint fmt,jint rate,jint channels,jbyteArray){
#ifdef WINAPK_HAVE_GSTREAMER
if((fmt!=1&&fmt!=2)||rate<=0||channels<=0)return 0;return (jlong)new Pass{rate,channels};
#else
return 0;
#endif
}
JNIEXPORT jbyteArray JNICALL Java_com_macapk_loader_NativeMedia_nativeDecDecode(JNIEnv*e,jclass,jlong h,jbyteArray in,jboolean eos){
#ifdef WINAPK_HAVE_GSTREAMER
if(!h)return nullptr;if(!in)return eos?e->NewByteArray(0):nullptr;jsize n=e->GetArrayLength(in);jbyteArray o=e->NewByteArray(n);if(n){std::vector<jbyte>b((size_t)n);e->GetByteArrayRegion(in,0,n,b.data());e->SetByteArrayRegion(o,0,n,b.data());}return o;
#else
(void)e;(void)h;(void)in;(void)eos;return nullptr;
#endif
}
JNIEXPORT jint JNICALL Java_com_macapk_loader_NativeMedia_nativeDecOutSampleRate(JNIEnv*,jclass,jlong h){
#ifdef WINAPK_HAVE_GSTREAMER
return h?((Pass*)h)->rate:0;
#else
return 0;
#endif
}
JNIEXPORT jint JNICALL Java_com_macapk_loader_NativeMedia_nativeDecOutChannels(JNIEnv*,jclass,jlong h){
#ifdef WINAPK_HAVE_GSTREAMER
return h?((Pass*)h)->channels:0;
#else
return 0;
#endif
}
JNIEXPORT void JNICALL Java_com_macapk_loader_NativeMedia_nativeDecClose(JNIEnv*,jclass,jlong h){
#ifdef WINAPK_HAVE_GSTREAMER
delete (Pass*)h;
#else
(void)h;
#endif
}
}


namespace winapk::host {
namespace {
constexpr const char* kNativeMediaClass = "com.macapk.loader.NativeMedia";
const HostMethod kNativeMediaMethods[] = {
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDemuxOpen", "(Ljava/lang/String;)J", Java_com_macapk_loader_NativeMedia_nativeDemuxOpen),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDemuxTrackCount", "(J)I", Java_com_macapk_loader_NativeMedia_nativeDemuxTrackCount),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDemuxSampleRate", "(J)I", Java_com_macapk_loader_NativeMedia_nativeDemuxSampleRate),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDemuxChannels", "(J)I", Java_com_macapk_loader_NativeMedia_nativeDemuxChannels),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDemuxFormatId", "(J)I", Java_com_macapk_loader_NativeMedia_nativeDemuxFormatId),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDemuxDurationUs", "(J)J", Java_com_macapk_loader_NativeMedia_nativeDemuxDurationUs),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDemuxCsd0", "(J)[B", Java_com_macapk_loader_NativeMedia_nativeDemuxCsd0),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDemuxCurrent", "(J)[B", Java_com_macapk_loader_NativeMedia_nativeDemuxCurrent),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDemuxSampleTime", "(J)J", Java_com_macapk_loader_NativeMedia_nativeDemuxSampleTime),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDemuxAdvance", "(J)I", Java_com_macapk_loader_NativeMedia_nativeDemuxAdvance),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDemuxSeek", "(JJ)V", Java_com_macapk_loader_NativeMedia_nativeDemuxSeek),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDemuxClose", "(J)V", Java_com_macapk_loader_NativeMedia_nativeDemuxClose),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDecOpen", "(III[B)J", Java_com_macapk_loader_NativeMedia_nativeDecOpen),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDecDecode", "(J[BZ)[B", Java_com_macapk_loader_NativeMedia_nativeDecDecode),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDecOutSampleRate", "(J)I", Java_com_macapk_loader_NativeMedia_nativeDecOutSampleRate),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDecOutChannels", "(J)I", Java_com_macapk_loader_NativeMedia_nativeDecOutChannels),
    WINAPK_HOST_NATIVE(kNativeMediaClass, "nativeDecClose", "(J)V", Java_com_macapk_loader_NativeMedia_nativeDecClose),
};
}  // namespace
HostMethods native_media_methods() {
    return {kNativeMediaMethods, sizeof(kNativeMediaMethods) / sizeof(kNativeMediaMethods[0])};
}
}  // namespace winapk::host
