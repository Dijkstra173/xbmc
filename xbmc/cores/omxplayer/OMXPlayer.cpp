/*
 *      Copyright (C) 2011-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "system.h"

#if defined (HAS_OMXPLAYER)

#include <sstream>
#include <iomanip>

#include "OMXPlayerAudio.h"
#include "OMXPlayerVideo.h"
#include "OMXPlayer.h"
#include "Application.h"
#include "ApplicationMessenger.h"
#include "GUIInfoManager.h"
#include "cores/VideoRenderers/RenderManager.h"
#include "cores/VideoRenderers/RenderFlags.h"
#include "FileItem.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "guilib/GUIWindowManager.h"
#include "settings/AdvancedSettings.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "threads/SingleLock.h"
#include "windowing/WindowingFactory.h"

#include "utils/log.h"
#include "utils/TimeUtils.h"
#include "utils/URIUtils.h"
#include "utils/Variant.h"
#include "utils/StreamDetails.h"
#include "xbmc/playlists/PlayListM3U.h"

#include "utils/LangCodeExpander.h"
#include "guilib/LocalizeStrings.h"
#include "guilib/Key.h"

#include "storage/MediaManager.h"
#include "GUIUserMessages.h"
#include "utils/StreamUtils.h"

#include "DVDInputStreams/DVDFactoryInputStream.h"
#include "DVDInputStreams/DVDInputStreamNavigator.h"
#include "DVDInputStreams/DVDInputStreamTV.h"
#include "DVDInputStreams/DVDInputStreamPVRManager.h"

#include "DVDDemuxers/DVDDemux.h"
#include "DVDDemuxers/DVDDemuxUtils.h"
#include "DVDDemuxers/DVDDemuxVobsub.h"
#include "DVDDemuxers/DVDFactoryDemuxer.h"
#include "DVDDemuxers/DVDDemuxFFmpeg.h"

#include "DVDCodecs/DVDCodecs.h"
#include "DVDCodecs/DVDFactoryCodec.h"

#include "DVDFileInfo.h"

#include "utils/LangCodeExpander.h"
#include "guilib/Key.h"
#include "guilib/LocalizeStrings.h"

#include "utils/URIUtils.h"
#include "GUIInfoManager.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/StereoscopicsManager.h"
#include "Application.h"
#include "ApplicationMessenger.h"
#include "filesystem/File.h"
#include "pictures/Picture.h"
#include "libswscale/swscale.h"
#ifdef HAS_VIDEO_PLAYBACK
#include "cores/VideoRenderers/RenderManager.h"
#endif
#ifdef HAS_PERFORMANCE_SAMPLE
#include "xbmc/utils/PerformanceSample.h"
#else
#define MEASURE_FUNCTION
#endif
#include "settings/AdvancedSettings.h"
#include "FileItem.h"
#include "GUIUserMessages.h"
#include "settings/Settings.h"
#include "settings/MediaSettings.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"
#include "utils/StreamDetails.h"
#include "pvr/PVRManager.h"
#include "pvr/channels/PVRChannel.h"
#include "pvr/windows/GUIWindowPVR.h"
#include "pvr/addons/PVRClients.h"
#include "filesystem/PVRFile.h"
#include "video/dialogs/GUIDialogFullScreenInfo.h"
#include "utils/StreamUtils.h"
#include "utils/Variant.h"
#include "storage/MediaManager.h"
#include "dialogs/GUIDialogBusy.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "xbmc/playlists/PlayListM3U.h"
#include "utils/StringUtils.h"
#include "Util.h"
#include "LangInfo.h"
#include "URL.h"
#include "utils/LangCodeExpander.h"

// video not playing from clock, but stepped
#define TP(speed)  ((speed) < 0 || (speed) > 4*DVD_PLAYSPEED_NORMAL)
// audio not playing
#define TPA(speed) ((speed) != DVD_PLAYSPEED_PAUSE && (speed) != DVD_PLAYSPEED_NORMAL)

using namespace std;
using namespace PVR;

void COMXSelectionStreams::Clear(StreamType type, StreamSource source)
{
  CSingleLock lock(m_section);
  for(int i=m_Streams.size()-1;i>=0;i--)
  {
    if(type && m_Streams[i].type != type)
      continue;

    if(source && m_Streams[i].source != source)
      continue;

    m_Streams.erase(m_Streams.begin() + i);
  }
}

OMXSelectionStream& COMXSelectionStreams::Get(StreamType type, int index)
{
  CSingleLock lock(m_section);
  int count = -1;
  for(int i=0;i<(int)m_Streams.size();i++)
  {
    if(m_Streams[i].type != type)
      continue;
    count++;
    if(count == index)
      return m_Streams[i];
  }
  CLog::Log(LOGERROR, "%s - failed to get stream", __FUNCTION__);
  return m_invalid;
}

std::vector<OMXSelectionStream> COMXSelectionStreams::Get(StreamType type)
{
  std::vector<OMXSelectionStream> streams;
  int count = Count(type);
  for(int index = 0; index < count; ++index){
    streams.push_back(Get(type, index));
  }
  return streams;
}

#define PREDICATE_RETURN(lh, rh) \
  do { \
    if((lh) != (rh)) \
      return (lh) > (rh); \
  } while(0)

class PredicateSubtitleFilter
{
private:
  std::string audiolang;
  bool original;
public:
  /** \brief The class' operator() decides if the given (subtitle) SelectionStream is relevant wrt.
  *          preferred subtitle language and audio language. If the subtitle is relevant <B>false</B> false is returned.
  *
  *          A subtitle is relevant if
  *          - it was previously selected, or
  *          - it's an external sub, or
  *          - it's a forced sub and "original stream's language" was selected, or
  *          - it's a forced sub and its language matches the audio's language, or
  *          - it's a default sub, or
  *          - its language matches the preferred subtitle's language (unequal to "original stream's language")
  */
  PredicateSubtitleFilter(std::string& lang)
    : audiolang(lang),
      original(StringUtils::EqualsNoCase(CSettings::Get().GetString("locale.subtitlelanguage"), "original"))
  {
  };

  bool operator()(const OMXSelectionStream& ss) const
  {
    if (ss.type_index == CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleStream)
      return false;

    if(STREAM_SOURCE_MASK(ss.source) == STREAM_SOURCE_DEMUX_SUB || STREAM_SOURCE_MASK(ss.source) == STREAM_SOURCE_TEXT)
      return false;

    if ((ss.flags & CDemuxStream::FLAG_FORCED) && (original || g_LangCodeExpander.CompareLangCodes(ss.language, audiolang)))
      return false;

    if ((ss.flags & CDemuxStream::FLAG_DEFAULT))
      return false;

    if(!original)
    {
      std::string subtitle_language = g_langInfo.GetSubtitleLanguage();
      if (g_LangCodeExpander.CompareLangCodes(subtitle_language, ss.language))
        return false;
    }

    return true;
  }
};

static bool PredicateAudioPriority(const OMXSelectionStream& lh, const OMXSelectionStream& rh)
{
  PREDICATE_RETURN(lh.type_index == CMediaSettings::Get().GetCurrentVideoSettings().m_AudioStream
                 , rh.type_index == CMediaSettings::Get().GetCurrentVideoSettings().m_AudioStream);

  // TrueHD never has enough cpu to decode on Pi, so prefer to avoid that
  PREDICATE_RETURN(lh.codec != "truehd"
                 , rh.codec != "truehd");

  if(!StringUtils::EqualsNoCase(CSettings::Get().GetString("locale.audiolanguage"), "original"))
  {
    CStdString audio_language = g_langInfo.GetAudioLanguage();
    PREDICATE_RETURN(g_LangCodeExpander.CompareLangCodes(audio_language, lh.language)
                   , g_LangCodeExpander.CompareLangCodes(audio_language, rh.language));
  }

  PREDICATE_RETURN(lh.flags & CDemuxStream::FLAG_DEFAULT
                 , rh.flags & CDemuxStream::FLAG_DEFAULT);

  PREDICATE_RETURN(lh.channels
                 , rh.channels);

  PREDICATE_RETURN(StreamUtils::GetCodecPriority(lh.codec)
                 , StreamUtils::GetCodecPriority(rh.codec));
  return false;
}

/** \brief The class' operator() decides if the given (subtitle) SelectionStream lh is 'better than' the given (subtitle) SelectionStream rh.
*          If lh is 'better than' rh the return value is true, false otherwise.
*
*          A subtitle lh is 'better than' a subtitle rh (in evaluation order) if
*          - lh was previously selected, or
*          - lh is an external sub and rh not, or
*          - lh is a forced sub and ("original stream's language" was selected or subtitles are off) and rh not, or
*          - lh is an external sub and its language matches the preferred subtitle's language (unequal to "original stream's language") and rh not, or
*          - lh is language matches the preferred subtitle's language (unequal to "original stream's language") and rh not, or
*          - lh is a default sub and rh not
*/
class PredicateSubtitlePriority
{
private:
  std::string audiolang;
  bool original;
  bool subson;
  PredicateSubtitleFilter filter;
public:
  PredicateSubtitlePriority(std::string& lang)
    : audiolang(lang),
      original(StringUtils::EqualsNoCase(CSettings::Get().GetString("locale.subtitlelanguage"), "original")),
      subson(CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleOn),
      filter(lang)
  {
  };

  bool relevant(const OMXSelectionStream& ss) const
  {
    return !filter(ss);
  }

  bool operator()(const OMXSelectionStream& lh, const OMXSelectionStream& rh) const
  {
    PREDICATE_RETURN(relevant(lh)
                   , relevant(rh));

    PREDICATE_RETURN(lh.type_index == CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleStream
                   , rh.type_index == CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleStream);

    // prefer external subs
    PREDICATE_RETURN(STREAM_SOURCE_MASK(lh.source) == STREAM_SOURCE_DEMUX_SUB
                   , STREAM_SOURCE_MASK(rh.source) == STREAM_SOURCE_DEMUX_SUB);

    PREDICATE_RETURN(STREAM_SOURCE_MASK(lh.source) == STREAM_SOURCE_TEXT
                   , STREAM_SOURCE_MASK(rh.source) == STREAM_SOURCE_TEXT);

    if(!subson || original)
    {
      PREDICATE_RETURN(lh.flags & CDemuxStream::FLAG_FORCED && g_LangCodeExpander.CompareLangCodes(lh.language, audiolang)
                     , rh.flags & CDemuxStream::FLAG_FORCED && g_LangCodeExpander.CompareLangCodes(rh.language, audiolang));

      PREDICATE_RETURN(lh.flags & CDemuxStream::FLAG_FORCED
                     , rh.flags & CDemuxStream::FLAG_FORCED);
    }

    CStdString subtitle_language = g_langInfo.GetSubtitleLanguage();
    if(!original)
    {
      PREDICATE_RETURN((STREAM_SOURCE_MASK(lh.source) == STREAM_SOURCE_DEMUX_SUB || STREAM_SOURCE_MASK(lh.source) == STREAM_SOURCE_TEXT) && g_LangCodeExpander.CompareLangCodes(subtitle_language, lh.language)
                     , (STREAM_SOURCE_MASK(rh.source) == STREAM_SOURCE_DEMUX_SUB || STREAM_SOURCE_MASK(rh.source) == STREAM_SOURCE_TEXT) && g_LangCodeExpander.CompareLangCodes(subtitle_language, rh.language));
    }

    if(!original)
    {
      PREDICATE_RETURN(g_LangCodeExpander.CompareLangCodes(subtitle_language, lh.language)
                     , g_LangCodeExpander.CompareLangCodes(subtitle_language, rh.language));
    }

    PREDICATE_RETURN(lh.flags & CDemuxStream::FLAG_DEFAULT
                   , rh.flags & CDemuxStream::FLAG_DEFAULT);

    return false;
  }
};

static bool PredicateVideoPriority(const OMXSelectionStream& lh, const OMXSelectionStream& rh)
{
  PREDICATE_RETURN(lh.flags & CDemuxStream::FLAG_DEFAULT
                 , rh.flags & CDemuxStream::FLAG_DEFAULT);
  return false;
}

bool COMXSelectionStreams::Get(StreamType type, CDemuxStream::EFlags flag, OMXSelectionStream& out)
{
  CSingleLock lock(m_section);
  for(int i=0;i<(int)m_Streams.size();i++)
  {
    if(m_Streams[i].type != type)
      continue;
    if((m_Streams[i].flags & flag) != flag)
      continue;
    out = m_Streams[i];
    return true;
  }
  return false;
}

int COMXSelectionStreams::IndexOf(StreamType type, int source, int id) const
{
  CSingleLock lock(m_section);
  int count = -1;
  for(int i=0;i<(int)m_Streams.size();i++)
  {
    if(type && m_Streams[i].type != type)
      continue;
    count++;
    if(source && m_Streams[i].source != source)
      continue;
    if(id < 0)
      continue;
    if(m_Streams[i].id == id)
      return count;
  }
  if(id < 0)
    return count;
  else
    return -1;
}

int COMXSelectionStreams::IndexOf(StreamType type, COMXPlayer& p) const
{
  if (p.m_pInputStream && p.m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
  {
    int id = -1;
    if(type == STREAM_AUDIO)
      id = ((CDVDInputStreamNavigator*)p.m_pInputStream)->GetActiveAudioStream();
    else if(type == STREAM_VIDEO)
      id = p.m_CurrentVideo.id;
    else if(type == STREAM_SUBTITLE)
      id = ((CDVDInputStreamNavigator*)p.m_pInputStream)->GetActiveSubtitleStream();

    return IndexOf(type, STREAM_SOURCE_NAV, id);
  }

  if(type == STREAM_AUDIO)
    return IndexOf(type, p.m_CurrentAudio.source, p.m_CurrentAudio.id);
  else if(type == STREAM_VIDEO)
    return IndexOf(type, p.m_CurrentVideo.source, p.m_CurrentVideo.id);
  else if(type == STREAM_SUBTITLE)
    return IndexOf(type, p.m_CurrentSubtitle.source, p.m_CurrentSubtitle.id);
  else if(type == STREAM_TELETEXT)
    return IndexOf(type, p.m_CurrentTeletext.source, p.m_CurrentTeletext.id);

  return -1;
}

int COMXSelectionStreams::Source(StreamSource source, std::string filename)
{
  CSingleLock lock(m_section);
  int index = source - 1;
  for(int i=0;i<(int)m_Streams.size();i++)
  {
    OMXSelectionStream &s = m_Streams[i];
    if(STREAM_SOURCE_MASK(s.source) != source)
      continue;
    // if it already exists, return same
    if(s.filename == filename)
      return s.source;
    if(index < s.source)
      index = s.source;
  }
  // return next index
  return index + 1;
}

void COMXSelectionStreams::Update(OMXSelectionStream& s)
{
  CSingleLock lock(m_section);
  int index = IndexOf(s.type, s.source, s.id);
  if(index >= 0)
  {
    OMXSelectionStream& o = Get(s.type, index);
    s.type_index = o.type_index;
    o = s;
  }
  else
  {
    s.type_index = Count(s.type);
    m_Streams.push_back(s);
  }
}

void COMXSelectionStreams::Update(CDVDInputStream* input, CDVDDemux* demuxer)
{
  if(input && input->IsStreamType(DVDSTREAM_TYPE_DVD))
  {
    CDVDInputStreamNavigator* nav = (CDVDInputStreamNavigator*)input;
    string filename = nav->GetFileName();
    int source = Source(STREAM_SOURCE_NAV, filename);

    int count;
    count = nav->GetAudioStreamCount();
    for(int i=0;i<count;i++)
    {
      OMXSelectionStream s;
      s.source   = source;
      s.type     = STREAM_AUDIO;
      s.id       = i;
      s.flags    = CDemuxStream::FLAG_NONE;
      s.filename = filename;

      DVDNavStreamInfo info;
      nav->GetAudioStreamInfo(i, info);
      s.name     = info.name;
      s.language = g_LangCodeExpander.ConvertToISO6392T(info.language);
      s.channels = info.channels;
      Update(s);
    }

    count = nav->GetSubTitleStreamCount();
    for(int i=0;i<count;i++)
    {
      OMXSelectionStream s;
      s.source   = source;
      s.type     = STREAM_SUBTITLE;
      s.id       = i;
      s.flags    = CDemuxStream::FLAG_NONE;
      s.filename = filename;
      s.channels = 0;

      DVDNavStreamInfo info;
      nav->GetSubtitleStreamInfo(i, info);
      s.name     = info.name;
      s.language = g_LangCodeExpander.ConvertToISO6392T(info.language);
      Update(s);
    }
  }
  else if(demuxer)
  {
    string filename = demuxer->GetFileName();
    int count = demuxer->GetNrOfStreams();
    int source;
    if(input) /* hack to know this is sub decoder */
      source = Source(STREAM_SOURCE_DEMUX, filename);
    else
      source = Source(STREAM_SOURCE_DEMUX_SUB, filename);


    for(int i=0;i<count;i++)
    {
      CDemuxStream* stream = demuxer->GetStream(i);
      /* skip streams with no type */
      if (stream->type == STREAM_NONE)
        continue;
      /* make sure stream is marked with right source */
      stream->source = source;

      OMXSelectionStream s;
      s.source   = source;
      s.type     = stream->type;
      s.id       = stream->iId;
      s.language = g_LangCodeExpander.ConvertToISO6392T(stream->language);
      s.flags    = stream->flags;
      s.filename = demuxer->GetFileName();
      stream->GetStreamName(s.name);
      CStdString codec;
      demuxer->GetStreamCodecName(stream->iId, codec);
      s.codec    = codec;
      s.channels = 0; // Default to 0. Overwrite if STREAM_AUDIO below.
      if(stream->type == STREAM_AUDIO)
      {
        std::string type;
        ((CDemuxStreamAudio*)stream)->GetStreamType(type);
        if(type.length() > 0)
        {
          if(s.name.length() > 0)
            s.name += " - ";
          s.name += type;
        }
        s.channels = ((CDemuxStreamAudio*)stream)->iChannels;
      }
      Update(s);
    }
  }
}

COMXPlayer::COMXPlayer(IPlayerCallback &callback) 
    : IPlayer(callback),
      CThread("OMXPlayer"),
      m_CurrentAudio(STREAM_AUDIO, DVDPLAYER_AUDIO),
      m_CurrentVideo(STREAM_VIDEO, DVDPLAYER_VIDEO),
      m_CurrentSubtitle(STREAM_SUBTITLE, DVDPLAYER_SUBTITLE),
      m_CurrentTeletext(STREAM_TELETEXT, DVDPLAYER_TELETEXT),
      m_messenger("player"),
      m_omxPlayerVideo(&m_av_clock, &m_overlayContainer, m_messenger),
      m_omxPlayerAudio(&m_av_clock, m_messenger),
      m_dvdPlayerSubtitle(&m_overlayContainer),
      m_dvdPlayerTeletext(),
      m_ready(true),
      m_DemuxerPausePending(false)
{
  m_pDemuxer          = NULL;
  m_pSubtitleDemuxer  = NULL;
  m_pInputStream      = NULL;

  m_dvd.Clear();
  m_State.Clear();
  m_EdlAutoSkipMarkers.Clear();
  m_UpdateApplication = 0;

  m_bAbortRequest = false;
  m_errorCount = 0;
  m_offset_pts = 0.0;
  m_playSpeed = DVD_PLAYSPEED_NORMAL;
  m_caching           = CACHESTATE_DONE;
  m_HasVideo          = false;
  m_HasAudio          = false;
  m_stepped           = false;
  m_video_fifo        = 0;
  m_audio_fifo        = 0;
  m_last_check_time   = 0.0;
  m_stamp             = 0.0;

  memset(&m_SpeedState, 0, sizeof(m_SpeedState));

#ifdef DVDDEBUG_MESSAGE_TRACKER
  g_dvdMessageTracker.Init();
#endif
}

COMXPlayer::~COMXPlayer()
{
  CloseFile();

#ifdef DVDDEBUG_MESSAGE_TRACKER
  g_dvdMessageTracker.DeInit();
#endif
}

bool COMXPlayer::OpenFile(const CFileItem &file, const CPlayerOptions &options)
{
  try
  {
    CLog::Log(LOGNOTICE, "COMXPlayer: Opening: %s", CURL::GetRedacted(file.GetPath()).c_str());

    // if playing a file close it first
    // this has to be changed so we won't have to close it.
    if(IsRunning())
      CloseFile();

    m_bAbortRequest = false;

    SetPlaySpeed(DVD_PLAYSPEED_NORMAL);

    m_State.Clear();
    m_UpdateApplication = 0;
    m_offset_pts        = 0;

    m_PlayerOptions = options;
    m_item              = file;
    m_mimetype          = file.GetMimeType();
    m_filename          = file.GetPath();

    m_ready.Reset();

#if defined(HAS_VIDEO_PLAYBACK)
    g_renderManager.PreInit();
#endif

    Create();

    // wait for the ready event
    CGUIDialogBusy::WaitOnEvent(m_ready, g_advancedSettings.m_videoBusyDialogDelay_ms, false);

    // Playback might have been stopped due to some error
    if (m_bStop || m_bAbortRequest)
      return false;

    return true;
  }
  catch(...)
  {
    CLog::Log(LOGERROR, "%s - Exception thrown on open", __FUNCTION__);
    return false;
  }
}

bool COMXPlayer::CloseFile(bool reopen)
{
  CLog::Log(LOGDEBUG, "COMXPlayer::CloseFile");

  // set the abort request so that other threads can finish up
  m_bAbortRequest = true;

  // tell demuxer to abort
  if(m_pDemuxer)
    m_pDemuxer->Abort();

  if(m_pSubtitleDemuxer)
    m_pSubtitleDemuxer->Abort();

  if(m_pInputStream)
    m_pInputStream->Abort();

  CLog::Log(LOGDEBUG, "COMXPlayer: waiting for threads to exit");

  // wait for the main thread to finish up
  // since this main thread cleans up all other resources and threads
  // we are done after the StopThread call
  StopThread();
  
  m_Edl.Clear();
  m_EdlAutoSkipMarkers.Clear();

  m_HasVideo = false;
  m_HasAudio = false;

  CLog::Log(LOGNOTICE, "COMXPlayer: finished waiting");
#if defined(HAS_VIDEO_PLAYBACK)
  g_renderManager.UnInit();
#endif
  return true;
}

bool COMXPlayer::IsPlaying() const
{
  return !m_bStop;
}

void COMXPlayer::OnStartup()
{
  m_CurrentVideo.Clear();
  m_CurrentAudio.Clear();
  m_CurrentSubtitle.Clear();
  m_CurrentTeletext.Clear();

  m_messenger.Init();

  CUtil::ClearTempFonts();
}

bool COMXPlayer::OpenInputStream()
{
  if(m_pInputStream)
    SAFE_DELETE(m_pInputStream);

  CLog::Log(LOGNOTICE, "Creating InputStream");

  // correct the filename if needed
  CStdString filename(m_filename);
  if (StringUtils::StartsWith(filename, "dvd://")
  ||  StringUtils::EqualsNoCase(filename, "iso9660://video_ts/video_ts.ifo"))
  {
    m_filename = g_mediaManager.TranslateDevicePath("");
  }

  m_pInputStream = CDVDFactoryInputStream::CreateInputStream(this, m_filename, m_mimetype);
  if(m_pInputStream == NULL)
  {
    CLog::Log(LOGERROR, "COMXPlayer::OpenInputStream - unable to create input stream for [%s]", m_filename.c_str());
    return false;
  }
  else
    m_pInputStream->SetFileItem(m_item);

  if (!m_pInputStream->Open(m_filename.c_str(), m_mimetype))
  {
    CLog::Log(LOGERROR, "COMXPlayer::OpenInputStream - error opening [%s]", m_filename.c_str());
    return false;
  }

  if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD)
                       || m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY))
  {
    CLog::Log(LOGINFO, "COMXPlayer::OpenInputStream - DVD/BD not supported - Will try...");
  }

  // find any available external subtitles for non dvd files
  if (!m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD)
  &&  !m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER)
  &&  !m_pInputStream->IsStreamType(DVDSTREAM_TYPE_TV)
  &&  !m_pInputStream->IsStreamType(DVDSTREAM_TYPE_HTSP))
  {
    // find any available external subtitles
    std::vector<CStdString> filenames;
    CUtil::ScanForExternalSubtitles( m_filename, filenames );

    // find any upnp subtitles
    CStdString key("upnp:subtitle:1");
    for(unsigned s = 1; m_item.HasProperty(key); key = StringUtils::Format("upnp:subtitle:%u", ++s))
      filenames.push_back(m_item.GetProperty(key).asString());

    for(unsigned int i=0;i<filenames.size();i++)
    {
      // if vobsub subtitle:
      if (URIUtils::HasExtension(filenames[i], ".idx"))
      {
        CStdString strSubFile;
        if ( CUtil::FindVobSubPair( filenames, filenames[i], strSubFile ) )
          AddSubtitleFile(filenames[i], strSubFile);
      }
      else
      {
        if ( !CUtil::IsVobSub(filenames, filenames[i] ) )
        {
          AddSubtitleFile(filenames[i]);
        }
      }
    } // end loop over all subtitle files

    CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleCached = true;
  }

  SetAVDelay(CMediaSettings::Get().GetCurrentVideoSettings().m_AudioDelay);
  SetSubTitleDelay(CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleDelay);
  m_clock.Reset();
  m_dvd.Clear();
  m_errorCount = 0;
  m_ChannelEntryTimeOut.SetInfinite();

  return true;
}

bool COMXPlayer::OpenDemuxStream()
{
  if(m_pDemuxer)
    SAFE_DELETE(m_pDemuxer);

  CLog::Log(LOGNOTICE, "Creating Demuxer");

  try
  {
    int attempts = 10;
    while(!m_bStop && attempts-- > 0)
    {
      m_pDemuxer = CDVDFactoryDemuxer::CreateDemuxer(m_pInputStream);
      if(!m_pDemuxer && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER))
      {
        continue;
      }
      else if(!m_pDemuxer && m_pInputStream->NextStream() != CDVDInputStream::NEXTSTREAM_NONE)
      {
        CLog::Log(LOGDEBUG, "%s - New stream available from input, retry open", __FUNCTION__);
        continue;
      }
      break;
    }

    if(!m_pDemuxer)
    {
      CLog::Log(LOGERROR, "%s - Error creating demuxer", __FUNCTION__);
      return false;
    }

  }
  catch(...)
  {
    CLog::Log(LOGERROR, "%s - Exception thrown when opening demuxer", __FUNCTION__);
    return false;
  }

  m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_DEMUX);
  m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_NAV);
  m_SelectionStreams.Update(m_pInputStream, m_pDemuxer);

  int64_t len = m_pInputStream->GetLength();
  int64_t tim = m_pDemuxer->GetStreamLength();
  if(len > 0 && tim > 0)
    m_pInputStream->SetReadRate(g_advancedSettings.m_readBufferFactor * len * 1000 / tim);

  return true;
}

void COMXPlayer::OpenDefaultStreams(bool reset)
{
  // if input stream dictate, we will open later
  if(m_dvd.iSelectedAudioStream >= 0
  || m_dvd.iSelectedSPUStream   >= 0)
    return;

  OMXSelectionStreams streams;
  bool valid;

  // open video stream
  streams = m_SelectionStreams.Get(STREAM_VIDEO, PredicateVideoPriority);
  valid   = false;
  for(OMXSelectionStreams::iterator it = streams.begin(); it != streams.end() && !valid; ++it)
  {
    if(OpenVideoStream(it->id, it->source, reset))
      valid = true;
  }
  if(!valid)
    CloseVideoStream(true);

  // open audio stream
  if(m_PlayerOptions.video_only)
    streams.clear();
  else
    streams = m_SelectionStreams.Get(STREAM_AUDIO, PredicateAudioPriority);
  valid   = false;

  for(OMXSelectionStreams::iterator it = streams.begin(); it != streams.end() && !valid; ++it)
  {
    if(OpenAudioStream(it->id, it->source, reset))
      valid = true;
  }
  if(!valid)
    CloseAudioStream(true);

  // enable  or disable subtitles
  bool visible = CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleOn;

  // open subtitle stream
  OMXSelectionStream as = m_SelectionStreams.Get(STREAM_AUDIO, GetAudioStream());
  PredicateSubtitlePriority psp(as.language);
  streams = m_SelectionStreams.Get(STREAM_SUBTITLE, psp);
  valid   = false;
  for(OMXSelectionStreams::iterator it = streams.begin(); it != streams.end() && !valid; ++it)
  {
    if(OpenSubtitleStream(it->id, it->source))
    {
      valid = true;
      if(!psp.relevant(*it))
        visible = false;
      else if(it->flags & CDemuxStream::FLAG_FORCED)
        visible = true;
    }
  }
  if(!valid)
    CloseSubtitleStream(true);

  SetSubtitleVisibleInternal(visible);

  // open teletext stream
  streams = m_SelectionStreams.Get(STREAM_TELETEXT);
  valid   = false;
  for(OMXSelectionStreams::iterator it = streams.begin(); it != streams.end() && !valid; ++it)
  {
    if(OpenTeletextStream(it->id, it->source))
      valid = true;
  }
  if(!valid)
    CloseTeletextStream(true);
}

bool COMXPlayer::ReadPacket(DemuxPacket*& packet, CDemuxStream*& stream)
{

  // check if we should read from subtitle demuxer
  if( m_pSubtitleDemuxer && m_dvdPlayerSubtitle.AcceptsData() )
  {
    packet = m_pSubtitleDemuxer->Read();

    if(packet)
    {
      UpdateCorrection(packet, m_offset_pts);
      if(packet->iStreamId < 0)
        return true;

      stream = m_pSubtitleDemuxer->GetStream(packet->iStreamId);
      if (!stream)
      {
        CLog::Log(LOGERROR, "%s - Error demux packet doesn't belong to a valid stream", __FUNCTION__);
        return false;
      }
      if(stream->source == STREAM_SOURCE_NONE)
      {
        m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_DEMUX_SUB);
        m_SelectionStreams.Update(NULL, m_pSubtitleDemuxer);
      }
      return true;
    }
  }

  // read a data frame from stream.
  if(m_pDemuxer)
    packet = m_pDemuxer->Read();

  if(packet)
  {
    // stream changed, update and open defaults
    if(packet->iStreamId == DMX_SPECIALID_STREAMCHANGE)
    {
        m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_DEMUX);
        m_SelectionStreams.Update(m_pInputStream, m_pDemuxer);
        OpenDefaultStreams(false);

        // reevaluate HasVideo/Audio, we may have switched from/to a radio channel
        if(m_CurrentVideo.id < 0)
          m_HasVideo = false;
        if(m_CurrentAudio.id < 0)
          m_HasAudio = false;

        return true;
    }

    UpdateCorrection(packet, m_offset_pts);

    if(packet->iStreamId < 0)
      return true;

    if(m_pDemuxer)
    {
      stream = m_pDemuxer->GetStream(packet->iStreamId);
      if (!stream)
      {
        CLog::Log(LOGERROR, "%s - Error demux packet doesn't belong to a valid stream", __FUNCTION__);
        return false;
      }
      if(stream->source == STREAM_SOURCE_NONE)
      {
        m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_DEMUX);
        m_SelectionStreams.Update(m_pInputStream, m_pDemuxer);
      }
    }
    return true;
  }
  return false;
}

bool COMXPlayer::IsValidStream(COMXCurrentStream& stream)
{
  if(stream.id<0)
    return true; // we consider non selected as valid

  int source = STREAM_SOURCE_MASK(stream.source);
  if(source == STREAM_SOURCE_TEXT)
    return true;
  if(source == STREAM_SOURCE_DEMUX_SUB)
  {
    CDemuxStream* st = m_pSubtitleDemuxer->GetStream(stream.id);
    if(st == NULL || st->disabled)
      return false;
    if(st->type != stream.type)
      return false;
    return true;
  }
  if(source == STREAM_SOURCE_DEMUX)
  {
    CDemuxStream* st = m_pDemuxer->GetStream(stream.id);
    if(st == NULL || st->disabled)
      return false;
    if(st->type != stream.type)
      return false;

    if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
    {
      if(stream.type == STREAM_AUDIO    && st->iPhysicalId != m_dvd.iSelectedAudioStream)
        return false;
      if(stream.type == STREAM_SUBTITLE && st->iPhysicalId != m_dvd.iSelectedSPUStream)
        return false;
    }

    return true;
  }

  return false;
}

bool COMXPlayer::IsBetterStream(COMXCurrentStream& current, CDemuxStream* stream)
{
  // Do not reopen non-video streams if we're in video-only mode
  if(m_PlayerOptions.video_only && current.type != STREAM_VIDEO)
    return false;

  if(stream->disabled)
    return false;

  if (m_pInputStream && ( m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD)
                       || m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) ) )
  {
    int source_type;

    source_type = STREAM_SOURCE_MASK(current.source);
    if(source_type != STREAM_SOURCE_DEMUX
    && source_type != STREAM_SOURCE_NONE)
      return false;

    source_type = STREAM_SOURCE_MASK(stream->source);
    if(source_type  != STREAM_SOURCE_DEMUX
    || stream->type != current.type
    || stream->iId  == current.id)
      return false;

    if(current.type == STREAM_AUDIO    && stream->iPhysicalId == m_dvd.iSelectedAudioStream)
      return true;
    if(current.type == STREAM_SUBTITLE && stream->iPhysicalId == m_dvd.iSelectedSPUStream)
      return true;
    if(current.type == STREAM_VIDEO    && current.id < 0)
      return true;
  }
  else
  {
    if(stream->source == current.source
    && stream->iId    == current.id)
      return false;

    if(stream->type != current.type)
      return false;

    if(current.type == STREAM_SUBTITLE)
      return false;

    if(current.id < 0)
      return true;
  }
  return false;
}

static void SetSharpness(float sharpness)
{
  const int16_t mitchells[][32] =
  {
      { // B=1.66667 C=-0.33333
         0,  3,  8, 15, 24, 35, 49, 55, 70, 92,100,107,109,113,113,114,114,113,113,109,107,100, 92, 70, 55, 49, 35, 24, 15,  8,  3,  0,
      },
      { // B=1.64000 C=-0.32000
         0,  3,  7, 14, 24, 34, 48, 54, 69, 91,100,107,111,114,116,116,116,116,114,111,107,100, 91, 69, 54, 48, 34, 24, 14,  7,  3,  0,
      },
      { // B=1.61333 C=-0.30667
         0,  3,  7, 14, 23, 34, 47, 53, 68, 90, 99,107,112,115,118,118,118,118,115,112,107, 99, 90, 68, 53, 47, 34, 23, 14,  7,  3,  0,
      },
      { // B=1.58667 C=-0.29333
         0,  2,  7, 14, 22, 33, 46, 52, 67, 89, 99,107,113,117,119,121,121,119,117,113,107, 99, 89, 67, 52, 46, 33, 22, 14,  7,  2,  0,
      },
      { // B=1.56000 C=-0.28000
         0,  2,  7, 13, 22, 32, 45, 51, 66, 88, 98,107,114,119,121,123,123,121,119,114,107, 98, 88, 66, 51, 45, 32, 22, 13,  7,  2,  0,
      },
      { // B=1.53333 C=-0.26667
         0,  2,  7, 12, 21, 31, 44, 50, 65, 87, 98,108,114,120,123,125,125,123,120,114,108, 98, 87, 65, 50, 44, 31, 21, 12,  7,  2,  0,
      },
      { // B=1.50667 C=-0.25333
         0,  2,  6, 12, 20, 30, 43, 49, 64, 86, 98,108,116,122,125,127,127,125,122,116,108, 98, 86, 64, 49, 43, 30, 20, 12,  6,  2,  0,
      },
      { // B=1.48000 C=-0.24000
         0,  2,  6, 12, 19, 29, 42, 47, 63, 85, 98,108,117,123,127,130,130,127,123,117,108, 98, 85, 63, 47, 42, 29, 19, 12,  6,  2,  0,
      },
      { // B=1.45333 C=-0.22667
         0,  2,  6, 11, 19, 28, 41, 46, 62, 85, 97,108,118,125,129,132,132,129,125,118,108, 97, 85, 62, 46, 41, 28, 19, 11,  6,  2,  0,
      },
      { // B=1.42667 C=-0.21333
         0,  2,  5, 11, 18, 28, 40, 45, 61, 84, 97,108,119,126,131,134,134,131,126,119,108, 97, 84, 61, 45, 40, 28, 18, 11,  5,  2,  0,
      },
      { // B=1.40000 C=-0.20000
         0,  2,  5, 10, 18, 27, 39, 44, 60, 84, 96,109,119,127,134,136,136,134,127,119,109, 96, 84, 60, 44, 39, 27, 18, 10,  5,  2,  0,
      },
      { // B=1.37333 C=-0.18667
         0,  1,  5, 10, 17, 26, 38, 43, 58, 82, 96,109,120,129,135,139,139,135,129,120,109, 96, 82, 58, 43, 38, 26, 17, 10,  5,  1,  0,
      },
      { // B=1.34667 C=-0.17333
         0,  2,  4, 10, 16, 25, 37, 42, 57, 81, 96,109,121,131,137,141,141,137,131,121,109, 96, 81, 57, 42, 37, 25, 16, 10,  4,  2,  0,
      },
      { // B=1.32000 C=-0.16000
         0,  1,  4,  9, 15, 24, 36, 41, 56, 81, 95,110,122,132,139,143,143,139,132,122,110, 95, 81, 56, 41, 36, 24, 15,  9,  4,  1,  0,
      },
      { // B=1.29333 C=-0.14667
         0,  1,  4,  8, 15, 23, 35, 40, 55, 80, 95,110,123,133,141,146,146,141,133,123,110, 95, 80, 55, 40, 35, 23, 15,  8,  4,  1,  0,
      },
      { // B=1.26667 C=-0.13333
         0,  1,  4,  8, 14, 22, 33, 38, 54, 79, 95,110,124,135,143,148,148,143,135,124,110, 95, 79, 54, 38, 33, 22, 14,  8,  4,  1,  0,
      },
      { // B=1.24000 C=-0.12000
         0,  1,  4,  7, 14, 21, 33, 37, 53, 78, 94,110,125,136,145,150,150,145,136,125,110, 94, 78, 53, 37, 33, 21, 14,  7,  4,  1,  0,
      },
      { // B=1.21333 C=-0.10667
         0,  1,  3,  7, 13, 20, 32, 36, 52, 77, 94,110,127,138,147,152,152,147,138,127,110, 94, 77, 52, 36, 32, 20, 13,  7,  3,  1,  0,
      },
      { // B=1.18667 C=-0.09333
         0,  1,  3,  7, 12, 20, 30, 35, 51, 77, 93,111,125,140,149,155,155,149,140,125,111, 93, 77, 51, 35, 30, 20, 12,  7,  3,  1,  0,
      },
      { // B=1.16000 C=-0.08000
         0,  1,  3,  6, 11, 19, 29, 34, 50, 76, 93,111,128,141,151,157,157,151,141,128,111, 93, 76, 50, 34, 29, 19, 11,  6,  3,  1,  0,
      },
      { // B=1.13333 C=-0.06667
         0,  1,  3,  5, 11, 18, 28, 33, 49, 75, 93,111,129,143,153,159,159,153,143,129,111, 93, 75, 49, 33, 28, 18, 11,  5,  3,  1,  0,
      },
      { // B=1.10667 C=-0.05333
         0,  1,  2,  5, 10, 17, 27, 32, 48, 74, 93,111,130,144,155,161,161,155,144,130,111, 93, 74, 48, 32, 27, 17, 10,  5,  2,  1,  0,
      },
      { // B=1.08000 C=-0.04000
         0,  1,  2,  5,  9, 16, 26, 31, 46, 73, 92,112,130,145,157,164,164,157,145,130,112, 92, 73, 46, 31, 26, 16,  9,  5,  2,  1,  0,
      },
      { // B=1.05333 C=-0.02667
         0,  0,  2,  4,  9, 15, 25, 29, 45, 72, 92,112,131,147,159,166,166,159,147,131,112, 92, 72, 45, 29, 25, 15,  9,  4,  2,  0,  0,
      },
      { // B=1.02667 C=-0.01333
         0,  0,  1,  4,  8, 14, 24, 28, 44, 72, 92,112,132,148,161,168,168,161,148,132,112, 92, 72, 44, 28, 24, 14,  8,  4,  1,  0,  0,
      },
      { // B=1.00000 C=0.00000
         0,  0,  1,  4,  7, 14, 23, 27, 43, 71, 91,112,133,150,163,170,170,163,150,133,112, 91, 71, 43, 27, 23, 14,  7,  4,  1,  0,  0,
      },
      { // B=0.97333 C=0.01333
         0,  0,  1,  3,  7, 12, 22, 26, 42, 70, 91,113,133,152,165,173,173,165,152,133,113, 91, 70, 42, 26, 22, 12,  7,  3,  1,  0,  0,
      },
      { // B=0.94667 C=0.02667
         0,  0,  1,  2,  6, 12, 21, 25, 41, 69, 90,113,135,153,167,175,175,167,153,135,113, 90, 69, 41, 25, 21, 12,  6,  2,  1,  0,  0,
      },
      { // B=0.92000 C=0.04000
         0,  0,  0,  2,  5, 11, 20, 24, 40, 68, 90,113,136,154,169,177,177,169,154,136,113, 90, 68, 40, 24, 20, 11,  5,  2,  0,  0,  0,
      },
      { // B=0.89333 C=0.05333
         0,  0,  0,  1,  5, 10, 19, 23, 39, 67, 90,114,136,156,171,179,179,171,156,136,114, 90, 67, 39, 23, 19, 10,  5,  1,  0,  0,  0,
      },
      { // B=0.86667 C=0.06667
         0,  0,  0,  1,  4,  9, 18, 22, 38, 66, 89,114,137,157,173,182,182,173,157,137,114, 89, 66, 38, 22, 18,  9,  4,  1,  0,  0,  0,
      },
      { // B=0.84000 C=0.08000
         0,  0, -1,  1,  3,  9, 17, 21, 37, 65, 89,114,138,159,175,184,184,175,159,138,114, 89, 65, 37, 21, 17,  9,  3,  1, -1,  0,  0,
      },
      { // B=0.81333 C=0.09333
         0,  0, -1,  0,  3,  7, 16, 19, 36, 65, 89,114,139,160,177,186,186,177,160,139,114, 89, 65, 36, 19, 16,  7,  3,  0, -1,  0,  0,
      },
      { // B=0.78667 C=0.10667
         0, -1, -1,  0,  2,  6, 15, 18, 35, 64, 88,115,139,162,179,188,188,179,162,139,115, 88, 64, 35, 18, 15,  6,  2,  0, -1, -1,  0,
      },
      { // B=0.76000 C=0.12000
         0, -1, -1, -1,  1,  6, 14, 17, 33, 63, 88,115,141,163,181,191,191,181,163,141,115, 88, 63, 33, 17, 14,  6,  1, -1, -1, -1,  0,
      },
      { // B=0.73333 C=0.13333
         0, -1, -1, -1,  0,  5, 13, 16, 32, 62, 87,115,142,165,183,193,193,183,165,142,115, 87, 62, 32, 16, 13,  5,  0, -1, -1, -1,  0,
      },
      { // B=0.70667 C=0.14667
         0, -1, -1, -2,  0,  4, 12, 15, 31, 61, 87,115,143,166,185,195,195,185,166,143,115, 87, 61, 31, 15, 12,  4,  0, -2, -1, -1,  0,
      },
      { // B=0.68000 C=0.16000
         0, -1, -2, -2, -1,  3, 11, 14, 30, 61, 87,116,142,168,187,197,197,187,168,142,116, 87, 61, 30, 14, 11,  3, -1, -2, -2, -1,  0,
      },
      { // B=0.65333 C=0.17333
         0, -1, -2, -3, -1,  2, 10, 13, 29, 60, 86,116,144,169,189,200,200,189,169,144,116, 86, 60, 29, 13, 10,  2, -1, -3, -2, -1,  0,
      },
      { // B=0.62667 C=0.18667
         0, -1, -3, -3, -2,  1,  9, 12, 28, 59, 86,116,145,171,191,202,202,191,171,145,116, 86, 59, 28, 12,  9,  1, -2, -3, -3, -1,  0,
      },
      { // B=0.60000 C=0.20000
         0, -1, -3, -3, -3,  0,  8, 10, 27, 58, 86,116,146,172,193,204,204,193,172,146,116, 86, 58, 27, 10,  8,  0, -3, -3, -3, -1,  0,
      },
      { // B=0.57333 C=0.21333
         0, -1, -3, -4, -3, -1,  7,  9, 26, 57, 86,116,147,174,194,207,207,194,174,147,116, 86, 57, 26,  9,  7, -1, -3, -4, -3, -1,  0,
      },
      { // B=0.54667 C=0.22667
         0, -2, -3, -5, -4, -1,  5,  8, 25, 57, 85,117,148,176,196,209,209,196,176,148,117, 85, 57, 25,  8,  5, -1, -4, -5, -3, -2,  0,
      },
      { // B=0.52000 C=0.24000
         0, -1, -4, -5, -5, -2,  4,  7, 24, 55, 85,117,149,177,199,211,211,199,177,149,117, 85, 55, 24,  7,  4, -2, -5, -5, -4, -1,  0,
      },
      { // B=0.49333 C=0.25333
         0, -2, -4, -5, -6, -3,  3,  6, 23, 55, 84,117,150,178,200,214,214,200,178,150,117, 84, 55, 23,  6,  3, -3, -6, -5, -4, -2,  0,
      },
      { // B=0.46667 C=0.26667
         0, -2, -4, -6, -6, -4,  2,  6, 22, 54, 84,118,150,180,202,216,216,202,180,150,118, 84, 54, 22,  6,  2, -4, -6, -6, -4, -2,  0,
      },
      { // B=0.44000 C=0.28000
         0, -2, -4, -6, -7, -5,  2,  5, 21, 53, 83,118,150,181,205,218,218,205,181,150,118, 83, 53, 21,  5,  2, -5, -7, -6, -4, -2,  0,
      },
      { // B=0.41333 C=0.29333
         0, -2, -4, -7, -7, -6,  0,  5, 20, 53, 83,118,152,183,207,220,220,207,183,152,118, 83, 53, 20,  5,  0, -6, -7, -7, -4, -2,  0,
      },
      { // B=0.38667 C=0.30667
         0, -2, -5, -7, -8, -7, -1,  4, 19, 52, 83,118,153,185,208,223,223,208,185,153,118, 83, 52, 19,  4, -1, -7, -8, -7, -5, -2,  0,
      },
      { // B=0.36000 C=0.32000
         0, -2, -5, -8, -8, -8, -2,  3, 19, 51, 83,118,155,186,210,225,225,210,186,155,118, 83, 51, 19,  3, -2, -8, -8, -8, -5, -2,  0,
      },
      { // B=0.33333 C=0.33333
         0, -2, -6, -8,-10, -8, -3,  2, 18, 50, 82,119,155,187,213,227,227,213,187,155,119, 82, 50, 18,  2, -3, -8,-10, -8, -6, -2,  0,
      },
      { // B=0.32667 C=0.33667
         0, -2, -6, -8,-10, -8, -3,  2, 18, 49, 82,119,155,188,213,228,228,213,188,155,119, 82, 49, 18,  2, -3, -8,-10, -8, -6, -2,  0,
      },
      { // B=0.32000 C=0.34000
         0, -2, -6, -8,-10, -9, -3,  2, 18, 49, 82,119,155,188,214,228,228,214,188,155,119, 82, 49, 18,  2, -3, -9,-10, -8, -6, -2,  0,
      },
      { // B=0.31333 C=0.34333
         0, -2, -6, -8,-10, -9, -4,  1, 18, 49, 82,119,155,188,214,229,229,214,188,155,119, 82, 49, 18,  1, -4, -9,-10, -8, -6, -2,  0,
      },
      { // B=0.30667 C=0.34667
         0, -2, -6, -9,-10, -9, -4,  1, 18, 49, 82,119,156,189,214,229,229,214,189,156,119, 82, 49, 18,  1, -4, -9,-10, -9, -6, -2,  0,
      },
      { // B=0.30000 C=0.35000
         0, -3, -5, -9,-10,-10, -4,  1, 18, 49, 82,119,156,189,215,230,230,215,189,156,119, 82, 49, 18,  1, -4,-10,-10, -9, -5, -3,  0,
      },
      { // B=0.29333 C=0.35333
         0, -2, -6, -9,-10,-10, -4,  1, 17, 48, 82,119,156,190,215,231,231,215,190,156,119, 82, 48, 17,  1, -4,-10,-10, -9, -6, -2,  0,
      },
      { // B=0.28667 C=0.35667
         0, -2, -6, -9,-11,-10, -5,  1, 17, 48, 82,119,157,190,216,231,231,216,190,157,119, 82, 48, 17,  1, -5,-10,-11, -9, -6, -2,  0,
      },
      { // B=0.28000 C=0.36000
         0, -3, -6, -9,-11,-10, -5,  0, 17, 48, 82,119,157,190,217,231,231,217,190,157,119, 82, 48, 17,  0, -5,-10,-11, -9, -6, -3,  0,
      },
      { // B=0.27333 C=0.36333
         0, -3, -6, -9,-11,-11, -5,  0, 17, 48, 82,119,157,191,217,232,232,217,191,157,119, 82, 48, 17,  0, -5,-11,-11, -9, -6, -3,  0,
      },
      { // B=0.26667 C=0.36667
         0, -3, -6, -9,-11,-11, -5,  0, 17, 48, 81,119,157,191,217,233,233,217,191,157,119, 81, 48, 17,  0, -5,-11,-11, -9, -6, -3,  0,
      },
      { // B=0.26000 C=0.37000
         0, -3, -6,-10,-11,-11, -5,  0, 16, 47, 81,120,156,191,218,233,233,218,191,156,120, 81, 47, 16,  0, -5,-11,-11,-10, -6, -3,  0,
      },
      { // B=0.25333 C=0.37333
         0, -3, -6, -9,-12,-11, -6,  0, 16, 47, 81,119,158,192,218,234,234,218,192,158,119, 81, 47, 16,  0, -6,-11,-12, -9, -6, -3,  0,
      },
      { // B=0.24667 C=0.37667
         0, -3, -6,-10,-12,-11, -6,  0, 16, 47, 81,120,157,192,219,234,234,219,192,157,120, 81, 47, 16,  0, -6,-11,-12,-10, -6, -3,  0,
      },
      { // B=0.24000 C=0.38000
         0, -3, -6,-10,-12,-12, -6, -1, 16, 47, 81,120,158,193,219,235,235,219,193,158,120, 81, 47, 16, -1, -6,-12,-12,-10, -6, -3,  0,
      },
      { // B=0.23333 C=0.38333
         0, -3, -6,-10,-12,-12, -6, -1, 16, 46, 81,120,158,193,220,236,236,220,193,158,120, 81, 46, 16, -1, -6,-12,-12,-10, -6, -3,  0,
      },
      { // B=0.22667 C=0.38667
         0, -3, -6,-10,-12,-12, -7, -1, 15, 47, 80,120,158,194,220,236,236,220,194,158,120, 80, 47, 15, -1, -7,-12,-12,-10, -6, -3,  0,
      },
      { // B=0.22000 C=0.39000
         0, -3, -6,-10,-13,-12, -7, -1, 15, 46, 80,120,159,194,221,237,237,221,194,159,120, 80, 46, 15, -1, -7,-12,-13,-10, -6, -3,  0,
      },
      { // B=0.21333 C=0.39333
         0, -3, -6,-10,-13,-12, -8, -1, 15, 46, 80,120,159,194,221,237,237,221,194,159,120, 80, 46, 15, -1, -8,-12,-13,-10, -6, -3,  0,
      },
      { // B=0.20667 C=0.39667
         0, -3, -7,-10,-13,-12, -8, -2, 15, 46, 80,120,159,194,222,238,238,222,194,159,120, 80, 46, 15, -2, -8,-12,-13,-10, -7, -3,  0,
      },
      { // B=0.20000 C=0.40000
         0, -3, -7,-10,-13,-13, -8, -2, 15, 45, 81,120,159,195,222,238,238,222,195,159,120, 81, 45, 15, -2, -8,-13,-13,-10, -7, -3,  0,
      },
      { // B=0.19333 C=0.40333
         0, -3, -7,-11,-13,-13, -8, -2, 15, 45, 81,120,160,195,223,239,239,223,195,160,120, 81, 45, 15, -2, -8,-13,-13,-11, -7, -3,  0,
      },
      { // B=0.18667 C=0.40667
         0, -3, -7,-10,-14,-13, -9, -2, 14, 45, 80,120,160,196,223,240,240,223,196,160,120, 80, 45, 14, -2, -9,-13,-14,-10, -7, -3,  0,
      },
      { // B=0.18000 C=0.41000
         0, -3, -7,-11,-13,-13, -9, -2, 14, 45, 80,120,160,196,224,240,240,224,196,160,120, 80, 45, 14, -2, -9,-13,-13,-11, -7, -3,  0,
      },
      { // B=0.17333 C=0.41333
         0, -3, -7,-11,-13,-14, -9, -3, 14, 45, 80,120,160,196,225,240,240,225,196,160,120, 80, 45, 14, -3, -9,-14,-13,-11, -7, -3,  0,
      },
      { // B=0.16667 C=0.41667
         0, -3, -7,-11,-14,-14, -9, -3, 14, 44, 80,120,161,197,225,241,241,225,197,161,120, 80, 44, 14, -3, -9,-14,-14,-11, -7, -3,  0,
      },
      { // B=0.16000 C=0.42000
         0, -3, -7,-11,-14,-14,-10, -3, 14, 44, 80,120,161,197,225,242,242,225,197,161,120, 80, 44, 14, -3,-10,-14,-14,-11, -7, -3,  0,
      },
      { // B=0.15333 C=0.42333
         0, -3, -7,-11,-14,-14,-10, -3, 13, 44, 80,120,161,197,226,242,242,226,197,161,120, 80, 44, 13, -3,-10,-14,-14,-11, -7, -3,  0,
      },
      { // B=0.14667 C=0.42667
         0, -3, -7,-11,-15,-14,-10, -4, 14, 43, 80,120,163,198,226,243,243,226,198,163,120, 80, 43, 14, -4,-10,-14,-15,-11, -7, -3,  0,
      },
      { // B=0.14000 C=0.43000
         0, -3, -7,-12,-14,-15,-10, -4, 14, 43, 80,120,163,198,227,243,243,227,198,163,120, 80, 43, 14, -4,-10,-15,-14,-12, -7, -3,  0,
      },
      { // B=0.13333 C=0.43333
         0, -3, -7,-12,-14,-15,-11, -4, 13, 43, 79,121,161,199,227,244,244,227,199,161,121, 79, 43, 13, -4,-11,-15,-14,-12, -7, -3,  0,
      },
      { // B=0.12667 C=0.43667
         0, -3, -7,-12,-14,-15,-11, -4, 13, 43, 79,120,163,199,228,245,245,228,199,163,120, 79, 43, 13, -4,-11,-15,-14,-12, -7, -3,  0,
      },
      { // B=0.12000 C=0.44000
         0, -3, -7,-12,-15,-15,-12, -5, 13, 43, 79,121,162,199,228,245,245,228,199,162,121, 79, 43, 13, -5,-12,-15,-15,-12, -7, -3,  0,
      },
      { // B=0.11333 C=0.44333
         0, -3, -7,-12,-15,-16,-11, -5, 13, 42, 79,121,162,200,229,246,246,229,200,162,121, 79, 42, 13, -5,-11,-16,-15,-12, -7, -3,  0,
      },
      { // B=0.10667 C=0.44667
         0, -3, -8,-12,-15,-16,-12, -5, 13, 42, 79,121,162,200,229,246,246,229,200,162,121, 79, 42, 13, -5,-12,-16,-15,-12, -8, -3,  0,
      },
      { // B=0.10000 C=0.45000
         0, -3, -8,-12,-16,-16,-12, -6, 13, 42, 79,121,163,200,230,247,247,230,200,163,121, 79, 42, 13, -6,-12,-16,-16,-12, -8, -3,  0,
      },
      { // B=0.09333 C=0.45333
         0, -3, -8,-12,-16,-16,-13, -5, 12, 42, 79,121,163,201,230,248,248,230,201,163,121, 79, 42, 12, -5,-13,-16,-16,-12, -8, -3,  0,
      },
      { // B=0.08667 C=0.45667
         0, -3, -8,-12,-16,-16,-13, -5, 12, 41, 79,121,163,201,231,248,248,231,201,163,121, 79, 41, 12, -5,-13,-16,-16,-12, -8, -3,  0,
      },
      { // B=0.08000 C=0.46000
         0, -3, -8,-12,-16,-17,-13, -6, 12, 41, 79,121,163,201,232,248,248,232,201,163,121, 79, 41, 12, -6,-13,-17,-16,-12, -8, -3,  0,
      },
      { // B=0.07333 C=0.46333
         0, -3, -8,-13,-16,-17,-13, -6, 12, 41, 79,121,164,202,232,249,249,232,202,164,121, 79, 41, 12, -6,-13,-17,-16,-13, -8, -3,  0,
      },
      { // B=0.06667 C=0.46667
         0, -3, -8,-13,-16,-17,-14, -6, 11, 41, 79,121,164,202,233,249,249,233,202,164,121, 79, 41, 11, -6,-14,-17,-16,-13, -8, -3,  0,
      },
      { // B=0.06000 C=0.47000
         0, -3, -8,-13,-16,-18,-14, -6, 11, 40, 79,121,164,203,233,250,250,233,203,164,121, 79, 40, 11, -6,-14,-18,-16,-13, -8, -3,  0,
      },
      { // B=0.05333 C=0.47333
         0, -3, -8,-13,-17,-18,-14, -6, 11, 40, 79,121,165,203,233,251,251,233,203,165,121, 79, 40, 11, -6,-14,-18,-17,-13, -8, -3,  0,
      },
      { // B=0.04667 C=0.47667
         0, -4, -8,-13,-17,-18,-14, -7, 11, 40, 79,121,166,203,234,251,251,234,203,166,121, 79, 40, 11, -7,-14,-18,-17,-13, -8, -4,  0,
      },
      { // B=0.04000 C=0.48000
         0, -4, -8,-13,-17,-18,-14, -7, 11, 40, 78,121,166,204,234,251,251,234,204,166,121, 78, 40, 11, -7,-14,-18,-17,-13, -8, -4,  0,
      },
      { // B=0.03333 C=0.48333
         0, -4, -8,-14,-17,-18,-15, -7, 11, 40, 78,122,164,204,235,252,252,235,204,164,122, 78, 40, 11, -7,-15,-18,-17,-14, -8, -4,  0,
      },
      { // B=0.02667 C=0.48667
         0, -4, -8,-14,-17,-19,-15, -7, 10, 40, 78,122,164,205,235,253,253,235,205,164,122, 78, 40, 10, -7,-15,-19,-17,-14, -8, -4,  0,
      },
      { // B=0.02000 C=0.49000
         0, -4, -8,-14,-17,-19,-15, -7, 10, 39, 78,122,164,205,236,253,253,236,205,164,122, 78, 39, 10, -7,-15,-19,-17,-14, -8, -4,  0,
      },
      { // B=0.01333 C=0.49333
         0, -4, -8,-14,-18,-19,-16, -7,  9, 40, 78,122,165,205,236,254,254,236,205,165,122, 78, 40,  9, -7,-16,-19,-18,-14, -8, -4,  0,
      },
      { // B=0.00667 C=0.49667
         0, -4, -9,-14,-18,-19,-15, -8, 10, 39, 78,122,166,206,236,254,254,236,206,166,122, 78, 39, 10, -8,-15,-19,-18,-14, -9, -4,  0,
      },
      { // B=0.00000 C=0.50000
         0, -4, -8,-14,-18,-19,-16, -8,  9, 39, 77,122,166,206,237,255,255,237,206,166,122, 77, 39,  9, -8,-16,-19,-18,-14, -8, -4,  0,
      },
  };
  int index = (sharpness + 1.0f) * 50.0f + 0.5f;
  if (index >=0 && index <= 100)
  {
    const int16_t *coef = mitchells[index];

    char command[33*12];
    char response[33*12];
    unsigned int len = sprintf(command, "scaling_kernel ");
    for (int i=0; i < 32; i++) {
       if (len + 12 < sizeof command)
         len += sprintf(command+len, "%d ", coef[i]);
    }
    // no interpolate flag
    if (len + 12 < sizeof command)
      len += sprintf(command+len, " %d", 0);
    //printf("%i: %s\n", index, command);
    vc_gencmd(response, sizeof response, command);
  }
}

void COMXPlayer::Process()
{
  bool bOmxWaitVideo = false;
  bool bOmxWaitAudio = false;
  bool bOmxSentEOFs = false;
  float m_threshold = 0.2f;

  if (!OpenInputStream())
  {
    m_bAbortRequest = true;
    return;
  }

  if (CDVDInputStream::IMenus* ptr = dynamic_cast<CDVDInputStream::IMenus*>(m_pInputStream))
  {
    CLog::Log(LOGNOTICE, "OMXPlayer: playing a file with menu's");
    if(dynamic_cast<CDVDInputStreamNavigator*>(m_pInputStream))
      m_PlayerOptions.starttime = 0;

    if(m_PlayerOptions.state.size() > 0)
      ptr->SetState(m_PlayerOptions.state);
    else if(CDVDInputStreamNavigator* nav = dynamic_cast<CDVDInputStreamNavigator*>(m_pInputStream))
      nav->EnableSubtitleStream(CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleOn);

    CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleCached = true;
  }

  if(!OpenDemuxStream())
  {
    m_bAbortRequest = true;
    return;
  }

  // allow renderer to switch to fullscreen if requested
  m_omxPlayerVideo.EnableFullscreen(m_PlayerOptions.fullscreen);

  if(!m_av_clock.OMXInitialize(&m_clock))
  {
    m_bAbortRequest = true;
    return;
  }
  if(CSettings::Get().GetInt("videoplayer.adjustrefreshrate") != ADJUST_REFRESHRATE_OFF)
    m_av_clock.HDMIClockSync();
  m_av_clock.OMXStateIdle();
  m_av_clock.OMXStop();
  m_av_clock.OMXPause();

  OpenDefaultStreams();

  // look for any EDL files
  m_Edl.Clear();
  m_EdlAutoSkipMarkers.Clear();
  if (m_CurrentVideo.id >= 0 && m_CurrentVideo.hint.fpsrate > 0 && m_CurrentVideo.hint.fpsscale > 0)
  {
    float fFramesPerSecond = (float)m_CurrentVideo.hint.fpsrate / (float)m_CurrentVideo.hint.fpsscale;
    m_Edl.ReadEditDecisionLists(m_filename, fFramesPerSecond, m_CurrentVideo.hint.height);
  }

  /*
   * Check to see if the demuxer should start at something other than time 0. This will be the case
   * if there was a start time specified as part of the "Start from where last stopped" (aka
   * auto-resume) feature or if there is an EDL cut or commercial break that starts at time 0.
   */
  CEdl::Cut cut;
  int starttime = 0;
  if(m_PlayerOptions.starttime > 0 || m_PlayerOptions.startpercent > 0)
  {
    if (m_PlayerOptions.startpercent > 0 && m_pDemuxer)
    {
      int64_t playerStartTime = (int64_t) ( ( (float) m_pDemuxer->GetStreamLength() ) * ( m_PlayerOptions.startpercent/(float)100 ) );
      starttime = m_Edl.RestoreCutTime(playerStartTime);
    }
    else
    {
      starttime = m_Edl.RestoreCutTime((int64_t)m_PlayerOptions.starttime * 1000); // s to ms
    }
    CLog::Log(LOGDEBUG, "%s - Start position set to last stopped position: %d", __FUNCTION__, starttime);
  }
  else if(m_Edl.InCut(0, &cut)
      && (cut.action == CEdl::CUT || cut.action == CEdl::COMM_BREAK))
  {
    starttime = cut.end;
    CLog::Log(LOGDEBUG, "%s - Start position set to end of first cut or commercial break: %d", __FUNCTION__, starttime);
    if(cut.action == CEdl::COMM_BREAK)
    {
      /*
       * Setup auto skip markers as if the commercial break had been skipped using standard
       * detection.
       */
      m_EdlAutoSkipMarkers.commbreak_start = cut.start;
      m_EdlAutoSkipMarkers.commbreak_end   = cut.end;
      m_EdlAutoSkipMarkers.seek_to_start   = true;
    }
  }
  if(starttime > 0)
  {
    double startpts = DVD_NOPTS_VALUE;
    if(m_pDemuxer)
    {
      if (m_pDemuxer->SeekTime(starttime, false, &startpts))
        CLog::Log(LOGDEBUG, "%s - starting demuxer from: %d", __FUNCTION__, starttime);
      else
        CLog::Log(LOGDEBUG, "%s - failed to start demuxing from: %d", __FUNCTION__, starttime);
    }

    if(m_pSubtitleDemuxer)
    {
      if(m_pSubtitleDemuxer->SeekTime(starttime, false, &startpts))
        CLog::Log(LOGDEBUG, "%s - starting subtitle demuxer from: %d", __FUNCTION__, starttime);
      else
        CLog::Log(LOGDEBUG, "%s - failed to start subtitle demuxing from: %d", __FUNCTION__, starttime);
    }
  }

  // make sure all selected stream have data on startup
  if (CachePVRStream())
    SetCaching(CACHESTATE_PVR);

  // make sure application know our info
  UpdateApplication(0);
  UpdatePlayState(0);

  if(m_PlayerOptions.identify == false)
    m_callback.OnPlayBackStarted();

  // we are done initializing now, set the readyevent
  m_ready.Set();

  if (!CachePVRStream())
    SetCaching(CACHESTATE_FLUSH);

  EDEINTERLACEMODE current_deinterlace = CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode;
  float current_sharpness = CMediaSettings::Get().GetCurrentVideoSettings().m_Sharpness;
  SetSharpness(current_sharpness);

  while (!m_bAbortRequest)
  {
    double now = m_clock.GetAbsoluteClock();
    if (m_last_check_time == 0.0 || m_last_check_time + DVD_MSEC_TO_TIME(20) <= now)
    {
      m_last_check_time = now;
      m_stamp = m_av_clock.OMXMediaTime();
      const bool m_Pause = m_playSpeed == DVD_PLAYSPEED_PAUSE;
      const bool not_accepts_data = (!m_omxPlayerAudio.AcceptsData() && m_CurrentAudio.id >= 0) ||
          (!m_omxPlayerVideo.AcceptsData() && m_CurrentVideo.id >= 0);
      /* when the video/audio fifos are low, we pause clock, when high we resume */
      double audio_pts = floor(m_omxPlayerAudio.GetCurrentPts());
      double video_pts = floor(m_omxPlayerVideo.GetCurrentPts());

      float audio_fifo = audio_pts / DVD_TIME_BASE - m_stamp * 1e-6;
      float video_fifo = video_pts / DVD_TIME_BASE - m_stamp * 1e-6;
      float threshold = 0.1f;
      bool audio_fifo_low = false, video_fifo_low = false, audio_fifo_high = false, video_fifo_high = false;

      // if deinterlace setting has changed, we should close and open video
      if (current_deinterlace != CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode)
      {
        int iStream = m_CurrentVideo.id, source = m_CurrentVideo.source;
        CloseVideoStream(false);
        OpenVideoStream(iStream, source);
        if (m_State.canseek)
          m_messenger.Put(new CDVDMsgPlayerSeek(GetTime(), true, true, true, true, true));
        current_deinterlace = CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode;
      }

      // if sharpness setting has changed, we should update it
      if (current_sharpness != CMediaSettings::Get().GetCurrentVideoSettings().m_Sharpness)
      {
        current_sharpness = CMediaSettings::Get().GetCurrentVideoSettings().m_Sharpness;
        SetSharpness(current_sharpness);
      }

      m_video_fifo = (int)(100.0*(m_omxPlayerVideo.GetDecoderBufferSize()-m_omxPlayerVideo.GetDecoderFreeSpace())/m_omxPlayerVideo.GetDecoderBufferSize());
      m_audio_fifo = (int)(100.0*audio_fifo/m_omxPlayerAudio.GetCacheTotal());

      #ifdef _DEBUG
      static unsigned count;
      if ((count++ & 7) == 0)
      {
        char response[80];
        if (m_omxPlayerVideo.GetDecoderBufferSize() && m_omxPlayerAudio.GetCacheTotal())
          vc_gencmd(response, sizeof response, "render_bar 4 video_fifo %d %d %d %d",
              m_video_fifo,
              (int)(100.0*video_fifo/m_omxPlayerAudio.GetCacheTotal()),
              0, 100);
        if (m_omxPlayerAudio.GetCacheTotal())
          vc_gencmd(response, sizeof response, "render_bar 5 audio_fifo %d %d %d %d",
              m_audio_fifo,
              (int)(100.0*m_omxPlayerAudio.GetDelay()/m_omxPlayerAudio.GetCacheTotal()),
              0, 100);
        vc_gencmd(response, sizeof response, "render_bar 6 video_queue %d %d %d %d",
              m_omxPlayerVideo.GetLevel(), 0, 0, 100);
        vc_gencmd(response, sizeof response, "render_bar 7 audio_queue %d %d %d %d",
              m_omxPlayerAudio.GetLevel(), 0, 0, 100);
      }
      #endif
      if (audio_pts != DVD_NOPTS_VALUE)
      {
        audio_fifo_low = m_HasAudio && audio_fifo < threshold;
        audio_fifo_high = audio_pts != DVD_NOPTS_VALUE && audio_fifo >= m_threshold;
      }
      if (video_pts != DVD_NOPTS_VALUE)
      {
        video_fifo_low = m_HasVideo && video_fifo < threshold;
        video_fifo_high = video_pts != DVD_NOPTS_VALUE && video_fifo >= m_threshold;
      }
      if (!m_HasAudio && m_HasVideo)
        audio_fifo_high = true;
      if (!m_HasVideo && m_HasAudio)
        video_fifo_high = true;

      #ifdef _DEBUG
      CLog::Log(LOGDEBUG, "%s - M:%.6f-%.6f (A:%.6f V:%.6f) PEF:%d%d%d S:%.2f A:%.2f V:%.2f/T:%.2f (A:%d%d V:%d%d) A:%d%% V:%d%% (%.2f,%.2f)", __FUNCTION__,
        m_stamp*1e-6, m_av_clock.OMXClockAdjustment()*1e-6, audio_pts*1e-6, video_pts*1e-6, m_av_clock.OMXIsPaused(), bOmxSentEOFs, not_accepts_data, m_playSpeed * (1.0f/DVD_PLAYSPEED_NORMAL),
        audio_pts == DVD_NOPTS_VALUE ? 0.0:audio_fifo, video_pts == DVD_NOPTS_VALUE ? 0.0:video_fifo, m_threshold,
        audio_fifo_low, audio_fifo_high, video_fifo_low, video_fifo_high,
        m_omxPlayerAudio.GetLevel(), m_omxPlayerVideo.GetLevel(), m_omxPlayerAudio.GetDelay(), (float)m_omxPlayerAudio.GetCacheTotal());
      #endif

      if (TP(m_playSpeed))
      {
        if (m_CurrentVideo.started)
        {
          if (m_stamp == 0.0 && (!m_stepped || m_playSpeed > 0))
          {
            /* trickplay modes progress by stepping */
            CLog::Log(LOGDEBUG, "COMXPlayer::Process - Seeking step speed:%.2f last:%.2f v:%.2f", (double)m_playSpeed / DVD_PLAYSPEED_NORMAL, m_SpeedState.lastpts*1e-6, video_pts*1e-6);
            m_av_clock.OMXStep();
          }
          else
          {
            m_av_clock.OMXMediaTime(0.0);
            m_last_check_time = 0.0;
            m_stepped = true;
          }
        }
      }
      else if(!m_Pause && (bOmxSentEOFs || not_accepts_data || (audio_fifo_high && video_fifo_high)))
      {
        if (m_av_clock.OMXIsPaused())
        {
          CLog::Log(LOGDEBUG, "Resume %.2f,%.2f (A:%d%d V:%d%d) EOF:%d FULL:%d T:%.2f\n", audio_fifo, video_fifo,
            audio_fifo_low, audio_fifo_high, video_fifo_low, video_fifo_high, bOmxSentEOFs, not_accepts_data, m_threshold);
          m_av_clock.OMXStateExecute();
          m_av_clock.OMXResume();
        }
      }
      else if (m_Pause || audio_fifo_low || video_fifo_low)
      {
        if (!m_av_clock.OMXIsPaused() && !TPA(m_playSpeed))
        {
          if (!m_Pause)
            m_threshold = std::min(2.0f*m_threshold, 16.0f);
          CLog::Log(LOGDEBUG, "Pause %.2f,%.2f (A:%d%d V:%d%d) EOF:%d FULL:%d T:%.2f\n", audio_fifo, video_fifo,
            audio_fifo_low, audio_fifo_high, video_fifo_low, video_fifo_high, bOmxSentEOFs, not_accepts_data, m_threshold);
          m_av_clock.OMXPause();
        }
      }
    }
    HandleMessages();

    if(m_bAbortRequest)
      break;

    // should we open a new input stream?
    if(!m_pInputStream)
    {
      if (OpenInputStream() == false)
      {
        CLog::Log(LOGERROR, "%s - Closing stream due to OpenInputStream()", __FUNCTION__);
        m_bAbortRequest = true;
        break;
      }
    }

    // should we open a new demuxer?
    if(!m_pDemuxer)
    {
      if (m_pInputStream->NextStream() == CDVDInputStream::NEXTSTREAM_NONE)
        break;

      if (m_pInputStream->IsEOF())
        break;

      if (OpenDemuxStream() == false)
      {
        CLog::Log(LOGERROR, "%s - Closing stream due to OpenDemuxStream()", __FUNCTION__);
        m_bAbortRequest = true;
        break;
      }

      OpenDefaultStreams();

      // never allow first frames after open to be skipped
      if( m_omxPlayerVideo.IsInited() )
        m_omxPlayerVideo.SendMessage(new CDVDMsg(CDVDMsg::VIDEO_NOSKIP));

      if (CachePVRStream())
        SetCaching(CACHESTATE_PVR);

      UpdateApplication(0);
      UpdatePlayState(0);
    }

    // handle eventual seeks due to playspeed
    HandlePlaySpeed();

    // update player state
    UpdatePlayState(200);

    // update application with our state
    UpdateApplication(1000);

    // make sure we run subtitle process here
    m_dvdPlayerSubtitle.Process(m_clock.GetClock() + m_State.time_offset - m_omxPlayerVideo.GetSubtitleDelay(), m_State.time_offset);

    // OMX emergency exit
    if(HasAudio() && m_omxPlayerAudio.BadState())
    {
      CLog::Log(LOGERROR, "%s - Closing stream due to m_omxPlayerAudio.BadState()", __FUNCTION__);
      m_bAbortRequest = true;
      break;
    }

    if (CheckDelayedChannelEntry())
      continue;

    // if the queues are full, no need to read more
    if ((!m_omxPlayerAudio.AcceptsData() && m_CurrentAudio.id >= 0) ||
        (!m_omxPlayerVideo.AcceptsData() && m_CurrentVideo.id >= 0))
    {
      if(m_pDemuxer && m_DemuxerPausePending)
      {
        m_DemuxerPausePending = false;
        m_pDemuxer->SetSpeed(DVD_PLAYSPEED_PAUSE);
      }

      Sleep(10);
      continue;
    }

    // always yield to players if they have data levels > 50 percent
    if((m_omxPlayerAudio.GetLevel() > 50 || m_CurrentAudio.id < 0)
    && (m_omxPlayerVideo.GetLevel() > 50 || m_CurrentVideo.id < 0))
      Sleep(0);

    DemuxPacket* pPacket = NULL;
    CDemuxStream *pStream = NULL;
    ReadPacket(pPacket, pStream);
    if (pPacket && !pStream)
    {
      /* probably a empty packet, just free it and move on */
      CDVDDemuxUtils::FreeDemuxPacket(pPacket);
      continue;
    }
    if (pPacket)
    {
      // reset eos state when we get a packet (e.g. for case of seek after eos)
      bOmxWaitVideo = false;
      bOmxWaitAudio = false;
      bOmxSentEOFs = false;
    }
    if (!pPacket)
    {
      // when paused, demuxer could be be returning empty
      if (m_playSpeed == DVD_PLAYSPEED_PAUSE)
        continue;

      // check for a still frame state
      if (CDVDInputStream::IMenus* pStream = dynamic_cast<CDVDInputStream::IMenus*>(m_pInputStream))
      {
        // stills will be skipped
        if(m_dvd.state == DVDSTATE_STILL)
        {
          if (m_dvd.iDVDStillTime > 0)
          {
            if ((XbmcThreads::SystemClockMillis() - m_dvd.iDVDStillStartTime) >= m_dvd.iDVDStillTime)
            {
              m_dvd.iDVDStillTime = 0;
              m_dvd.iDVDStillStartTime = 0;
              m_dvd.state = DVDSTATE_NORMAL;
              pStream->SkipStill();
              continue;
            }
          }
        }
      }

      // if there is another stream available, reopen demuxer
      CDVDInputStream::ENextStream next = m_pInputStream->NextStream();
      if(next == CDVDInputStream::NEXTSTREAM_OPEN)
      {
        SAFE_DELETE(m_pDemuxer);
        m_CurrentAudio.stream = NULL;
        m_CurrentVideo.stream = NULL;
        m_CurrentSubtitle.stream = NULL;
        continue;
      }

      // input stream asked us to just retry
      if(next == CDVDInputStream::NEXTSTREAM_RETRY)
      {
        Sleep(100);
        continue;
      }

      // make sure we tell all players to finish it's data
      if (!bOmxSentEOFs)
      {
        if(m_CurrentAudio.inited)
        {
          m_omxPlayerAudio.SendMessage   (new CDVDMsg(CDVDMsg::GENERAL_EOF));
          bOmxWaitAudio = true;
        }
        if(m_CurrentVideo.inited)
        {
          m_omxPlayerVideo.SendMessage   (new CDVDMsg(CDVDMsg::GENERAL_EOF));
          bOmxWaitVideo = true;
        }
        if(m_CurrentSubtitle.inited)
          m_dvdPlayerSubtitle.SendMessage(new CDVDMsg(CDVDMsg::GENERAL_EOF));
        if(m_CurrentTeletext.inited)
          m_dvdPlayerTeletext.SendMessage(new CDVDMsg(CDVDMsg::GENERAL_EOF));
        m_CurrentAudio.inited    = false;
        m_CurrentVideo.inited    = false;
        m_CurrentSubtitle.inited = false;
        m_CurrentTeletext.inited = false;
        bOmxSentEOFs = true;
      }

      // if we are caching, start playing it again
      SetCaching(CACHESTATE_DONE);

      // while players are still playing, keep going to allow seekbacks
      if(m_omxPlayerVideo.HasData()
      || m_omxPlayerAudio.HasData())
      {
        Sleep(100);
        continue;
      }

      // wait for omx components to finish
      if(bOmxWaitVideo && !m_omxPlayerVideo.IsEOS())
      {
        Sleep(100);
        continue;
      }
      if(bOmxWaitAudio && !m_omxPlayerAudio.IsEOS())
      {
        Sleep(100);
        continue;
      }

      if (!m_pInputStream->IsEOF())
        CLog::Log(LOGINFO, "%s - eof reading from demuxer", __FUNCTION__);

      m_CurrentAudio.started    = false;
      m_CurrentVideo.started    = false;
      m_CurrentSubtitle.started = false;
      m_CurrentTeletext.started = false;

      break;
    }

    // it's a valid data packet, reset error counter
    m_errorCount = 0;

    // check so that none of our streams has become invalid
    if (!IsValidStream(m_CurrentAudio)    && m_omxPlayerAudio.IsStalled())    CloseAudioStream(true);
    if (!IsValidStream(m_CurrentVideo)    && m_omxPlayerVideo.IsStalled())    CloseVideoStream(true);
    if (!IsValidStream(m_CurrentSubtitle) && m_dvdPlayerSubtitle.IsStalled()) CloseSubtitleStream(true);
    if (!IsValidStream(m_CurrentTeletext))                                  CloseTeletextStream(true);

    // see if we can find something better to play
    if (IsBetterStream(m_CurrentAudio,    pStream)) OpenAudioStream   (pStream->iId, pStream->source);
    if (IsBetterStream(m_CurrentVideo,    pStream)) OpenVideoStream   (pStream->iId, pStream->source);
    if (IsBetterStream(m_CurrentSubtitle, pStream)) OpenSubtitleStream(pStream->iId, pStream->source);
    if (IsBetterStream(m_CurrentTeletext, pStream)) OpenTeletextStream(pStream->iId, pStream->source);

    // process the packet
    ProcessPacket(pStream, pPacket);

    // check if in a cut or commercial break that should be automatically skipped
    CheckAutoSceneSkip();
  }
}

bool COMXPlayer::CheckDelayedChannelEntry(void)
{
  bool bReturn(false);

  if (m_ChannelEntryTimeOut.IsTimePast())
  {
    CFileItem currentFile(g_application.CurrentFileItem());
    CPVRChannel *currentChannel = currentFile.GetPVRChannelInfoTag();
    SwitchChannel(*currentChannel);

    bReturn = true;
    m_ChannelEntryTimeOut.SetInfinite();
  }

  return bReturn;
}

void COMXPlayer::ProcessPacket(CDemuxStream* pStream, DemuxPacket* pPacket)
{
    /* process packet if it belongs to selected stream. for dvd's don't allow automatic opening of streams*/
    OMXStreamLock lock(this);

    try
    {
      if (pPacket->iStreamId == m_CurrentAudio.id && pStream->source == m_CurrentAudio.source && pStream->type == STREAM_AUDIO)
        ProcessAudioData(pStream, pPacket);
      else if (pPacket->iStreamId == m_CurrentVideo.id && pStream->source == m_CurrentVideo.source && pStream->type == STREAM_VIDEO)
        ProcessVideoData(pStream, pPacket);
      else if (pPacket->iStreamId == m_CurrentSubtitle.id && pStream->source == m_CurrentSubtitle.source && pStream->type == STREAM_SUBTITLE)
        ProcessSubData(pStream, pPacket);
      else if (pPacket->iStreamId == m_CurrentTeletext.id && pStream->source == m_CurrentTeletext.source && pStream->type == STREAM_TELETEXT)
        ProcessTeletextData(pStream, pPacket);
      else
      {
        pStream->SetDiscard(AVDISCARD_ALL);
        CDVDDemuxUtils::FreeDemuxPacket(pPacket); // free it since we won't do anything with it
      }
    }
    catch(...)
    {
      CLog::Log(LOGERROR, "%s - Exception thrown when processing demux packet", __FUNCTION__);
    }

}

void COMXPlayer::ProcessAudioData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  if (m_CurrentAudio.stream  != (void*)pStream
  ||  m_CurrentAudio.changes != pStream->changes)
  {
    /* check so that dmuxer hints or extra data hasn't changed */
    /* if they have, reopen stream */

    if (m_CurrentAudio.hint != CDVDStreamInfo(*pStream, true))
      OpenAudioStream( pPacket->iStreamId, pStream->source );

    m_CurrentAudio.stream = (void*)pStream;
    m_CurrentAudio.changes = pStream->changes;
  }

  // check if we are too slow and need to recache
  CheckStartCaching(m_CurrentAudio);

  CheckContinuity(m_CurrentAudio, pPacket);
  UpdateTimestamps(m_CurrentAudio, pPacket);

  bool drop = false;
  if (CheckPlayerInit(m_CurrentAudio, DVDPLAYER_AUDIO))
    drop = true;

  /*
   * If CheckSceneSkip() returns true then demux point is inside an EDL cut and the packets are dropped.
   * If not inside a hard cut, but the demux point has reached an EDL mute section then trigger the
   * AUDIO_SILENCE state. The AUDIO_SILENCE state is reverted as soon as the demux point is outside
   * of any EDL section while EDL mute is still active.
   */
  CEdl::Cut cut;
  if (CheckSceneSkip(m_CurrentAudio))
    drop = true;
  else if (m_Edl.InCut(DVD_TIME_TO_MSEC(m_CurrentAudio.dts + m_offset_pts), &cut) && cut.action == CEdl::MUTE // Inside EDL mute
  &&      !m_EdlAutoSkipMarkers.mute) // Mute not already triggered
  {
    m_omxPlayerAudio.SendMessage(new CDVDMsgBool(CDVDMsg::AUDIO_SILENCE, true));
    m_EdlAutoSkipMarkers.mute = true;
  }
  else if (!m_Edl.InCut(DVD_TIME_TO_MSEC(m_CurrentAudio.dts + m_offset_pts), &cut) // Outside of any EDL
  &&        m_EdlAutoSkipMarkers.mute) // But the mute hasn't been removed yet
  {
    m_omxPlayerAudio.SendMessage(new CDVDMsgBool(CDVDMsg::AUDIO_SILENCE, false));
    m_EdlAutoSkipMarkers.mute = false;
  }

  m_omxPlayerAudio.SendMessage(new CDVDMsgDemuxerPacket(pPacket, drop));
}

void COMXPlayer::ProcessVideoData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  if (m_CurrentVideo.stream != (void*)pStream
  ||  m_CurrentVideo.changes != pStream->changes)
  {
    /* check so that dmuxer hints or extra data hasn't changed */
    /* if they have reopen stream */

    if (m_CurrentVideo.hint != CDVDStreamInfo(*pStream, true))
      OpenVideoStream(pPacket->iStreamId, pStream->source);

    m_CurrentVideo.stream = (void*)pStream;
    m_CurrentVideo.changes = pStream->changes;
  }

  // check if we are too slow and need to recache
  CheckStartCaching(m_CurrentVideo);

  if( pPacket->iSize != 4) //don't check the EOF_SEQUENCE of stillframes
  {
    CheckContinuity(m_CurrentVideo, pPacket);
    UpdateTimestamps(m_CurrentVideo, pPacket);
  }

  bool drop = false;
  if (CheckPlayerInit(m_CurrentVideo, DVDPLAYER_VIDEO))
    drop = true;

  if (CheckSceneSkip(m_CurrentVideo))
    drop = true;

  m_omxPlayerVideo.SendMessage(new CDVDMsgDemuxerPacket(pPacket, drop));
}

void COMXPlayer::ProcessSubData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  if (m_CurrentSubtitle.stream != (void*)pStream
  ||  m_CurrentSubtitle.changes != pStream->changes)
  {
    /* check so that dmuxer hints or extra data hasn't changed */
    /* if they have reopen stream */

    if (m_CurrentSubtitle.hint != CDVDStreamInfo(*pStream, true))
      OpenSubtitleStream(pPacket->iStreamId, pStream->source);

    m_CurrentSubtitle.stream = (void*)pStream;
    m_CurrentSubtitle.changes = pStream->changes;
  }

  UpdateTimestamps(m_CurrentSubtitle, pPacket);

  bool drop = false;
  if (CheckPlayerInit(m_CurrentSubtitle, DVDPLAYER_SUBTITLE))
    drop = true;

  if (CheckSceneSkip(m_CurrentSubtitle))
    drop = true;

  m_dvdPlayerSubtitle.SendMessage(new CDVDMsgDemuxerPacket(pPacket, drop));

  if(m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
    m_dvdPlayerSubtitle.UpdateOverlayInfo((CDVDInputStreamNavigator*)m_pInputStream, LIBDVDNAV_BUTTON_NORMAL);
}

void COMXPlayer::ProcessTeletextData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  if (m_CurrentTeletext.stream  != (void*)pStream
  ||  m_CurrentTeletext.changes != pStream->changes)
  {
    /* check so that dmuxer hints or extra data hasn't changed */
    /* if they have, reopen stream */
    if (m_CurrentTeletext.hint != CDVDStreamInfo(*pStream, true))
      OpenTeletextStream( pPacket->iStreamId, pStream->source );

    m_CurrentTeletext.stream = (void*)pStream;
    m_CurrentTeletext.changes = pStream->changes;
  }
  UpdateTimestamps(m_CurrentTeletext, pPacket);

  bool drop = false;
  if (CheckPlayerInit(m_CurrentTeletext, DVDPLAYER_TELETEXT))
    drop = true;

  if (CheckSceneSkip(m_CurrentTeletext))
    drop = true;

  m_dvdPlayerTeletext.SendMessage(new CDVDMsgDemuxerPacket(pPacket, drop));
}

bool COMXPlayer::GetCachingTimes(double& level, double& delay, double& offset)
{
  if(!m_pInputStream || !m_pDemuxer)
    return false;

  XFILE::SCacheStatus status;
  if (!m_pInputStream->GetCacheStatus(&status))
    return false;

  int64_t cached   = status.forward;
  unsigned currate = status.currate;
  unsigned maxrate = status.maxrate;
  bool full        = status.full;

  int64_t length  = m_pInputStream->GetLength();
  int64_t remain  = length - m_pInputStream->Seek(0, SEEK_CUR);

  if(cached < 0 || length <= 0 || remain < 0)
    return false;

  double play_sbp  = DVD_MSEC_TO_TIME(m_pDemuxer->GetStreamLength()) / length;
  double queued = 1000.0 * GetQueueTime() / play_sbp;

  delay  = 0.0;
  level  = 0.0;
  offset = (double)(cached + queued) / length;

  if (currate == 0)
    return true;

  double cache_sbp   = 1.1 * (double)DVD_TIME_BASE / currate;         /* underestimate by 10 % */
  double play_left   = play_sbp  * (remain + queued);                 /* time to play out all remaining bytes */
  double cache_left  = cache_sbp * (remain - cached);                 /* time to cache the remaining bytes */
  double cache_need  = std::max(0.0, remain - play_left / cache_sbp); /* bytes needed until play_left == cache_left */

  delay = cache_left - play_left;

  if (full && (currate < maxrate) )
    level = -1.0;                          /* buffer is full & our read rate is too low  */
  else
    level = (cached + queued) / (cache_need + queued);

  return true;
}

void COMXPlayer::HandlePlaySpeed()
{
  ECacheState caching = m_caching;

  if(IsInMenu() && caching != CACHESTATE_DONE)
    caching = CACHESTATE_DONE;

  if(caching == CACHESTATE_FULL)
  {
    double level, delay, offset;
    if(GetCachingTimes(level, delay, offset))
    {
      if(level  < 0.0)
      {
        CGUIDialogKaiToast::QueueNotification(g_localizeStrings.Get(21454), g_localizeStrings.Get(21455));
        caching = CACHESTATE_INIT;
      }
      if(level >= 1.0)
        caching = CACHESTATE_INIT;
    }
    else
    {
      if ((!m_omxPlayerAudio.AcceptsData() && m_CurrentAudio.id >= 0)
      ||  (!m_omxPlayerVideo.AcceptsData() && m_CurrentVideo.id >= 0))
        caching = CACHESTATE_INIT;
    }
  }

  if(caching == CACHESTATE_INIT)
  {
    // if all enabled streams have been inited we are done
    if((m_CurrentVideo.id < 0 || m_CurrentVideo.started)
    && (m_CurrentAudio.id < 0 || m_CurrentAudio.started))
      caching = CACHESTATE_PLAY;

    // handle situation that we get no data on one stream
    if(m_CurrentAudio.id >= 0 && m_CurrentVideo.id >= 0)
    {
      if ((!m_omxPlayerAudio.AcceptsData() && !m_CurrentVideo.started)
      ||  (!m_omxPlayerVideo.AcceptsData() && !m_CurrentAudio.started))
      {
        caching = CACHESTATE_DONE;
      }
    }
  }

  if (caching == CACHESTATE_PVR)
  {
    bool bGotAudio(m_pDemuxer->GetNrOfAudioStreams() > 0);
    bool bGotVideo(m_pDemuxer->GetNrOfVideoStreams() > 0);
    bool bAudioLevelOk(m_omxPlayerAudio.GetLevel() > g_advancedSettings.m_iPVRMinAudioCacheLevel);
    bool bVideoLevelOk(m_omxPlayerVideo.GetLevel() > g_advancedSettings.m_iPVRMinVideoCacheLevel);
    bool bAudioFull(!m_omxPlayerAudio.AcceptsData());
    bool bVideoFull(!m_omxPlayerVideo.AcceptsData());

    if (/* if all streams got at least g_advancedSettings.m_iPVRMinCacheLevel in their buffers, we're done */
        ((bGotVideo || bGotAudio) && (!bGotAudio || bAudioLevelOk) && (!bGotVideo || bVideoLevelOk)) ||
        /* or if one of the buffers is full */
        (bAudioFull || bVideoFull))
    {
      CLog::Log(LOGDEBUG, "set caching from pvr to done. audio (%d) = %d. video (%d) = %d",
          bGotAudio, m_omxPlayerAudio.GetLevel(),
          bGotVideo, m_omxPlayerVideo.GetLevel());

      CFileItem currentItem(g_application.CurrentFileItem());
      if (currentItem.HasPVRChannelInfoTag())
        g_PVRManager.LoadCurrentChannelSettings();

      caching = CACHESTATE_DONE;
    }
    else
    {
      /* ensure that automatically started players are stopped while caching */
      if (m_CurrentAudio.started)
        m_omxPlayerAudio.SetSpeed(DVD_PLAYSPEED_PAUSE);
      if (m_CurrentVideo.started)
        m_omxPlayerVideo.SetSpeed(DVD_PLAYSPEED_PAUSE);
    }
  }

  if(caching == CACHESTATE_PLAY)
  {
    // if all enabled streams have started playing we are done
    if((m_CurrentVideo.id < 0 || !m_omxPlayerVideo.IsStalled())
    && (m_CurrentAudio.id < 0 || !m_omxPlayerAudio.IsStalled()))
      caching = CACHESTATE_DONE;
  }

  if(m_caching != caching)
    SetCaching(caching);


  if(GetPlaySpeed() != DVD_PLAYSPEED_NORMAL && GetPlaySpeed() != DVD_PLAYSPEED_PAUSE)
  {
    if (IsInMenu())
    {
      // this can't be done in menu
      SetPlaySpeed(DVD_PLAYSPEED_NORMAL);

    }
    else if (m_CurrentVideo.id >= 0
          &&  m_CurrentVideo.inited == true
          &&  m_SpeedState.lastpts  != m_omxPlayerVideo.GetCurrentPts()
          &&  m_SpeedState.lasttime != GetTime()
          &&  m_stepped)
    {
      m_SpeedState.lastpts  = m_omxPlayerVideo.GetCurrentPts();
      m_SpeedState.lasttime = GetTime();
      // check how much off clock video is when ff/rw:ing
      // a problem here is that seeking isn't very accurate
      // and since the clock will be resynced after seek
      // we might actually not really be playing at the wanted
      // speed. we'd need to have some way to not resync the clock
      // after a seek to remember timing. still need to handle
      // discontinuities somehow

      // when seeking, give the player a headstart to make sure
      // the time it takes to seek doesn't make a difference.
      double error;
      error  = m_clock.GetClock() - m_SpeedState.lastpts;
      error *= m_playSpeed / abs(m_playSpeed);

      if(error > DVD_MSEC_TO_TIME(1000))
      {
        CLog::Log(LOGDEBUG, "COMXPlayer::Process - Seeking to catch up");
        int64_t iTime = (int64_t)DVD_TIME_TO_MSEC(m_clock.GetClock() + m_State.time_offset + 500000.0 * m_playSpeed / DVD_PLAYSPEED_NORMAL);
        m_messenger.Put(new CDVDMsgPlayerSeek(iTime, (GetPlaySpeed() < 0), true, false, false, true));
      }
    }
  }
}

bool COMXPlayer::CheckStartCaching(COMXCurrentStream& current)
{
  if(m_caching   != CACHESTATE_DONE
  || m_playSpeed != DVD_PLAYSPEED_NORMAL)
    return false;

  if(IsInMenu())
    return false;

  if((current.type == STREAM_AUDIO && m_omxPlayerAudio.IsStalled())
  || (current.type == STREAM_VIDEO && m_omxPlayerVideo.IsStalled()))
  {
    if (CachePVRStream())
    {
      if ((current.type == STREAM_AUDIO && current.started && m_omxPlayerAudio.GetLevel() == 0) ||
         (current.type == STREAM_VIDEO && current.started && m_omxPlayerVideo.GetLevel() == 0))
      {
        CLog::Log(LOGDEBUG, "%s stream stalled. start buffering", current.type == STREAM_AUDIO ? "audio" : "video");
        SetCaching(CACHESTATE_PVR);
      }
      return true;
    }

    // don't start caching if it's only a single stream that has run dry
    if(m_omxPlayerAudio.GetLevel() > 50
    || m_omxPlayerVideo.GetLevel() > 50)
      return false;

    if(current.inited)
      SetCaching(CACHESTATE_FULL);
    else
      SetCaching(CACHESTATE_INIT);
    return true;
  }
  return false;
}

bool COMXPlayer::CheckPlayerInit(COMXCurrentStream& current, unsigned int source)
{
  if(current.inited)
    return false;

  if(current.startpts != DVD_NOPTS_VALUE)
  {
    if(current.dts == DVD_NOPTS_VALUE)
    {
      CLog::Log(LOGDEBUG, "%s - dropping packet type:%d dts:%f to get to start point at %f", __FUNCTION__, source,  current.dts, current.startpts);
      return true;
    }

    if((current.startpts - current.dts) > DVD_SEC_TO_TIME(20))
    {
      CLog::Log(LOGDEBUG, "%s - too far to decode before finishing seek", __FUNCTION__);
      if(m_CurrentAudio.startpts != DVD_NOPTS_VALUE)
        m_CurrentAudio.startpts = current.dts;
      if(m_CurrentVideo.startpts != DVD_NOPTS_VALUE)
        m_CurrentVideo.startpts = current.dts;
      if(m_CurrentSubtitle.startpts != DVD_NOPTS_VALUE)
        m_CurrentSubtitle.startpts = current.dts;
      if(m_CurrentTeletext.startpts != DVD_NOPTS_VALUE)
        m_CurrentTeletext.startpts = current.dts;
    }

    if(current.dts < current.startpts)
    {
      CLog::Log(LOGDEBUG, "%s - dropping packet type:%d dts:%f to get to start point at %f", __FUNCTION__, source,  current.dts, current.startpts);
      return true;
    }
  }

  //If this is the first packet after a discontinuity, send it as a resync
  if (current.dts != DVD_NOPTS_VALUE)
  {
    current.inited   = true;
    current.startpts = current.dts;

    bool setclock = false;
    if(m_playSpeed == DVD_PLAYSPEED_NORMAL)
    {
      if(     source == DVDPLAYER_AUDIO)
        setclock = !m_CurrentVideo.inited;
      else if(source == DVDPLAYER_VIDEO)
        setclock = !m_CurrentAudio.inited;
    }
    else
    {
      if(source == DVDPLAYER_VIDEO)
        setclock = true;
    }

    double starttime = current.startpts;
    if(m_CurrentAudio.inited
    && m_CurrentAudio.startpts != DVD_NOPTS_VALUE
    && m_CurrentAudio.startpts < starttime)
      starttime = m_CurrentAudio.startpts;
    if(m_CurrentVideo.inited
    && m_CurrentVideo.startpts != DVD_NOPTS_VALUE
    && m_CurrentVideo.startpts < starttime)
      starttime = m_CurrentVideo.startpts;

    starttime = current.startpts - starttime;
    if(starttime > 0 && setclock)
    {
      if(starttime > DVD_SEC_TO_TIME(2))
        CLog::Log(LOGWARNING, "COMXPlayer::CheckPlayerInit(%d) - Ignoring too large delay of %f", source, starttime);
      else
        SendPlayerMessage(new CDVDMsgDouble(CDVDMsg::GENERAL_DELAY, starttime), source);
    }

    SendPlayerMessage(new CDVDMsgGeneralResync(current.dts, setclock), source);
  }
  return false;
}

void COMXPlayer::UpdateCorrection(DemuxPacket* pkt, double correction)
{
  //CLog::Log(LOGINFO,"%s: %d dts:%.0f pts:%.0f s:%d c:%.0f (%d,%d)", __func__, (int)pkt->iStreamId, pkt->dts, pkt->pts, pkt->iSize, correction, pkt->dts==DVD_NOPTS_VALUE, pkt->pts==DVD_NOPTS_VALUE);
  if(pkt->dts != DVD_NOPTS_VALUE) pkt->dts -= correction;
  if(pkt->pts != DVD_NOPTS_VALUE) pkt->pts -= correction;
}

void COMXPlayer::UpdateTimestamps(COMXCurrentStream& current, DemuxPacket* pPacket)
{
  double dts = current.dts;
  /* update stored values */
  if(pPacket->dts != DVD_NOPTS_VALUE)
    dts = pPacket->dts;
  else if(pPacket->pts != DVD_NOPTS_VALUE)
    dts = pPacket->pts;

  /* calculate some average duration */
  if(pPacket->duration != DVD_NOPTS_VALUE)
    current.dur = pPacket->duration;
  else if(dts != DVD_NOPTS_VALUE && current.dts != DVD_NOPTS_VALUE)
    current.dur = 0.1 * (current.dur * 9 + (dts - current.dts));

  current.dts = dts;

  /* send a playback state structure periodically */
  if(current.dts_state == DVD_NOPTS_VALUE
  || abs(current.dts - current.dts_state) > DVD_MSEC_TO_TIME(200))
  {
    current.dts_state = current.dts;

    if (current.inited)
    {
      // make sure we send no outdated state to a/v players
      UpdatePlayState(0);
      SendPlayerMessage(new CDVDMsgType<SPlayerState>(CDVDMsg::PLAYER_DISPLAYTIME, m_StateInput), current.player);
    }
    else
    {
      CSingleLock lock(m_StateSection);
      m_State = m_StateInput;
    }
  }
}

static void UpdateLimits(double& minimum, double& maximum, double dts)
{
  if(dts == DVD_NOPTS_VALUE)
    return;
  if(minimum == DVD_NOPTS_VALUE || minimum > dts) minimum = dts;
  if(maximum == DVD_NOPTS_VALUE || maximum < dts) maximum = dts;
}

void COMXPlayer::CheckContinuity(COMXCurrentStream& current, DemuxPacket* pPacket)
{
return;
  if (m_playSpeed < DVD_PLAYSPEED_PAUSE)
    return;

  if( pPacket->dts == DVD_NOPTS_VALUE || current.dts == DVD_NOPTS_VALUE)
    return;

  double mindts = DVD_NOPTS_VALUE, maxdts = DVD_NOPTS_VALUE;
  UpdateLimits(mindts, maxdts, m_CurrentAudio.dts);
  UpdateLimits(mindts, maxdts, m_CurrentVideo.dts);
  UpdateLimits(mindts, maxdts, m_CurrentAudio.dts_end());
  UpdateLimits(mindts, maxdts, m_CurrentVideo.dts_end());

  /* if we don't have max and min, we can't do anything more */
  if( mindts == DVD_NOPTS_VALUE || maxdts == DVD_NOPTS_VALUE )
    return;

  double correction = 0.0;
  if( pPacket->dts > maxdts + DVD_MSEC_TO_TIME(1000))
  {
    CLog::Log(LOGDEBUG, "COMXPlayer::CheckContinuity - resync forward :%d, prev:%f, curr:%f, diff:%f"
                            , current.type, current.dts, pPacket->dts, pPacket->dts - maxdts);
    correction = pPacket->dts - maxdts;
  }

  /* if it's large scale jump, correct for it */
  if(pPacket->dts + DVD_MSEC_TO_TIME(100) < current.dts_end())
  {
    CLog::Log(LOGDEBUG, "COMXPlayer::CheckContinuity - resync backward :%d, prev:%f, curr:%f, diff:%f"
                            , current.type, current.dts, pPacket->dts, pPacket->dts - current.dts);
    correction = pPacket->dts - current.dts_end();
  }
  else if(pPacket->dts < current.dts)
  {
    CLog::Log(LOGDEBUG, "COMXPlayer::CheckContinuity - wrapback :%d, prev:%f, curr:%f, diff:%f"
                            , current.type, current.dts, pPacket->dts, pPacket->dts - current.dts);
  }

  if(correction != 0.0)
  {
    /* disable detection on next packet on other stream to avoid ping pong-ing */
    if(m_CurrentAudio.player != current.player) m_CurrentAudio.dts = DVD_NOPTS_VALUE;
    if(m_CurrentVideo.player != current.player) m_CurrentVideo.dts = DVD_NOPTS_VALUE;

    m_offset_pts += correction;
    UpdateCorrection(pPacket, correction);
  }
}

bool COMXPlayer::CheckSceneSkip(COMXCurrentStream& current)
{
  if(!m_Edl.HasCut())
    return false;

  if(current.dts == DVD_NOPTS_VALUE)
    return false;

  if(current.inited == false)
    return false;

  CEdl::Cut cut;
  return m_Edl.InCut(DVD_TIME_TO_MSEC(current.dts + m_offset_pts), &cut) && cut.action == CEdl::CUT;
}

void COMXPlayer::CheckAutoSceneSkip()
{
  if(!m_Edl.HasCut())
    return;

  /*
   * Check that there is an audio and video stream.
   */
  if(m_CurrentAudio.id < 0
  || m_CurrentVideo.id < 0)
    return;

  /*
   * If there is a startpts defined for either the audio or video stream then dvdplayer is still
   * still decoding frames to get to the previously requested seek point.
   */
  if(m_CurrentAudio.inited == false
  || m_CurrentVideo.inited == false)
    return;

  if(m_CurrentAudio.dts == DVD_NOPTS_VALUE
  || m_CurrentVideo.dts == DVD_NOPTS_VALUE)
    return;

  const int64_t clock = GetTime();

  CEdl::Cut cut;
  if(!m_Edl.InCut(clock, &cut))
    return;

  if(cut.action == CEdl::CUT
  && !(cut.end == m_EdlAutoSkipMarkers.cut || cut.start == m_EdlAutoSkipMarkers.cut)) // To prevent looping if same cut again
  {
    CLog::Log(LOGDEBUG, "%s - Clock in EDL cut [%s - %s]: %s. Automatically skipping over.",
              __FUNCTION__, CEdl::MillisecondsToTimeString(cut.start).c_str(),
              CEdl::MillisecondsToTimeString(cut.end).c_str(), CEdl::MillisecondsToTimeString(clock).c_str());
    /*
     * Seeking either goes to the start or the end of the cut depending on the play direction.
     */
    int64_t seek = GetPlaySpeed() >= 0 ? cut.end : cut.start;
    /*
     * Seeking is NOT flushed so any content up to the demux point is retained when playing forwards.
     */
    m_messenger.Put(new CDVDMsgPlayerSeek((int)seek, true, true, true, false, true));
    /*
     * Seek doesn't always work reliably. Last physical seek time is recorded to prevent looping
     * if there was an error with seeking and it landed somewhere unexpected, perhaps back in the
     * cut. The cut automatic skip marker is reset every 500ms allowing another attempt at the seek.
     */
    m_EdlAutoSkipMarkers.cut = GetPlaySpeed() >= 0 ? cut.end : cut.start;
  }
  else if(cut.action == CEdl::COMM_BREAK
  &&      GetPlaySpeed() >= 0
  &&      cut.start > m_EdlAutoSkipMarkers.commbreak_end)
  {
    CLog::Log(LOGDEBUG, "%s - Clock in commercial break [%s - %s]: %s. Automatically skipping to end of commercial break (only done once per break)",
              __FUNCTION__, CEdl::MillisecondsToTimeString(cut.start).c_str(), CEdl::MillisecondsToTimeString(cut.end).c_str(),
              CEdl::MillisecondsToTimeString(clock).c_str());
    /*
     * Seeking is NOT flushed so any content up to the demux point is retained when playing forwards.
     */
    m_messenger.Put(new CDVDMsgPlayerSeek(cut.end + 1, true, true, true, false, true));
    /*
     * Each commercial break is only skipped once so poorly detected commercial breaks can be
     * manually re-entered. Start and end are recorded to prevent looping and to allow seeking back
     * to the start of the commercial break if incorrectly flagged.
     */
    m_EdlAutoSkipMarkers.commbreak_start = cut.start;
    m_EdlAutoSkipMarkers.commbreak_end   = cut.end;
    m_EdlAutoSkipMarkers.seek_to_start   = true; // Allow backwards Seek() to go directly to the start
  }
}

void COMXPlayer::SynchronizeDemuxer(unsigned int timeout)
{
  if(IsCurrentThread())
    return;
  if(!m_messenger.IsInited())
    return;

  CDVDMsgGeneralSynchronize* message = new CDVDMsgGeneralSynchronize(timeout, 0);
  m_messenger.Put(message->Acquire());
  message->Wait(&m_bStop, 0);
  message->Release();
}

void COMXPlayer::SynchronizePlayers(unsigned int sources)
{
  /* we need a big timeout as audio queue is about 8seconds for 2ch ac3 */
  const int timeout = 10*1000; // in milliseconds

  CDVDMsgGeneralSynchronize* message = new CDVDMsgGeneralSynchronize(timeout, sources);
  if (m_CurrentAudio.id >= 0)
    m_omxPlayerAudio.SendMessage(message->Acquire());

  if (m_CurrentVideo.id >= 0)
    m_omxPlayerVideo.SendMessage(message->Acquire());
/* TODO - we have to rewrite the sync class, to not require
          all other players waiting for subtitle, should only
          be the oposite way
  if (m_CurrentSubtitle.id >= 0)
    m_dvdPlayerSubtitle.SendMessage(message->Acquire());
*/
  message->Release();
}

void COMXPlayer::SendPlayerMessage(CDVDMsg* pMsg, unsigned int target)
{
  if(target == DVDPLAYER_AUDIO)
    m_omxPlayerAudio.SendMessage(pMsg);
  if(target == DVDPLAYER_VIDEO)
    m_omxPlayerVideo.SendMessage(pMsg);
  if(target == DVDPLAYER_SUBTITLE)
    m_dvdPlayerSubtitle.SendMessage(pMsg);
  if(target == DVDPLAYER_TELETEXT)
    m_dvdPlayerTeletext.SendMessage(pMsg);
}

void COMXPlayer::OnExit()
{
  try
  {
    CLog::Log(LOGNOTICE, "COMXPlayer::OnExit()");

    m_av_clock.OMXStop();
    m_av_clock.OMXStateIdle();

    // set event to inform openfile something went wrong in case openfile is still waiting for this event
    SetCaching(CACHESTATE_DONE);

    // close each stream
    if (!m_bAbortRequest) CLog::Log(LOGNOTICE, "OMXPlayer: eof, waiting for queues to empty");
    if (m_CurrentAudio.id >= 0)
    {
      CLog::Log(LOGNOTICE, "OMXPlayer: closing audio stream");
      CloseAudioStream(!m_bAbortRequest);
    }
    if (m_CurrentVideo.id >= 0)
    {
      CLog::Log(LOGNOTICE, "OMXPlayer: closing video stream");
      CloseVideoStream(!m_bAbortRequest);
    }
    if (m_CurrentSubtitle.id >= 0)
    {
      CLog::Log(LOGNOTICE, "OMXPlayer: closing subtitle stream");
      CloseSubtitleStream(!m_bAbortRequest);
    }
    if (m_CurrentTeletext.id >= 0)
    {
      CLog::Log(LOGNOTICE, "OMXPlayer: closing teletext stream");
      CloseTeletextStream(!m_bAbortRequest);
    }
    // destroy the demuxer
    if (m_pDemuxer)
    {
      CLog::Log(LOGNOTICE, "COMXPlayer::OnExit() deleting demuxer");
      delete m_pDemuxer;
    }
    m_pDemuxer = NULL;

    if (m_pSubtitleDemuxer)
    {
      CLog::Log(LOGNOTICE, "COMXPlayer::OnExit() deleting subtitle demuxer");
      delete m_pSubtitleDemuxer;
    }
    m_pSubtitleDemuxer = NULL;

    // destroy the inputstream
    if (m_pInputStream)
    {
      CLog::Log(LOGNOTICE, "COMXPlayer::OnExit() deleting input stream");
      delete m_pInputStream;
    }
    m_pInputStream = NULL;

    // clean up all selection streams
    m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_NONE);

    m_messenger.End();

    m_av_clock.OMXDeinitialize();

  }
  catch (...)
  {
    CLog::Log(LOGERROR, "%s - Exception thrown when trying to close down player, memory leak will follow", __FUNCTION__);
    m_pInputStream = NULL;
    m_pDemuxer = NULL;
  }

  m_bStop = true;
  // if we didn't stop playing, advance to the next item in xbmc's playlist
  if(m_PlayerOptions.identify == false)
  {
    if (m_bAbortRequest)
      m_callback.OnPlayBackStopped();
    else
      m_callback.OnPlayBackEnded();
  }

  // set event to inform openfile something went wrong in case openfile is still waiting for this event
  m_ready.Set();
}

void COMXPlayer::HandleMessages()
{
  CDVDMsg* pMsg;
  OMXStreamLock lock(this);

  while (m_messenger.Get(&pMsg, 0) == MSGQ_OK)
  {

    try
    {
      if (pMsg->IsType(CDVDMsg::PLAYER_SEEK) && m_messenger.GetPacketCount(CDVDMsg::PLAYER_SEEK)         == 0
                                             && m_messenger.GetPacketCount(CDVDMsg::PLAYER_SEEK_CHAPTER) == 0)
      {
        CDVDMsgPlayerSeek &msg(*((CDVDMsgPlayerSeek*)pMsg));

        if (!m_State.canseek)
        {
          pMsg->Release();
          continue;
        }

        if(!msg.GetTrickPlay())
        {
          g_infoManager.SetDisplayAfterSeek(100000);
        }
        if(msg.GetFlush())
          SetCaching(CACHESTATE_FLUSH);

        double start = DVD_NOPTS_VALUE;

        int time = msg.GetRestore() ? (int)m_Edl.RestoreCutTime(msg.GetTime()) : msg.GetTime();

        // if input streams doesn't support seektime we must convert back to clock
        if(dynamic_cast<CDVDInputStream::ISeekTime*>(m_pInputStream) == NULL)
          time -= DVD_TIME_TO_MSEC(m_State.time_offset - m_offset_pts);

        CLog::Log(LOGDEBUG, "demuxer seek to: %d", time);
        if (m_pDemuxer && m_pDemuxer->SeekTime(time, msg.GetBackward(), &start))
        {
          CLog::Log(LOGDEBUG, "demuxer seek to: %.0f, success", start);
          if(m_pSubtitleDemuxer)
          {
            if(!m_pSubtitleDemuxer->SeekTime(time, msg.GetBackward()))
              CLog::Log(LOGDEBUG, "failed to seek subtitle demuxer: %d, success", time);
          }
          // dts after successful seek
          if (m_StateInput.time_src  == ETIMESOURCE_CLOCK && start == DVD_NOPTS_VALUE)
            m_StateInput.dts = DVD_MSEC_TO_TIME(time);
          else
            m_StateInput.dts = start;

          FlushBuffers(!msg.GetFlush(), start, msg.GetAccurate());
          // mark mediatime as invalid
          m_av_clock.OMXMediaTime(0.0);
          m_last_check_time = 0.0;
        }
        else
          CLog::Log(LOGWARNING, "error while seeking");

        // set flag to indicate we have finished a seeking request
        if(!msg.GetTrickPlay())
          g_infoManager.SetDisplayAfterSeek();

        // dvd's will issue a HOP_CHANNEL that we need to skip
        if(m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
          m_dvd.state = DVDSTATE_SEEK;
      }
      else if (pMsg->IsType(CDVDMsg::PLAYER_SEEK_CHAPTER) && m_messenger.GetPacketCount(CDVDMsg::PLAYER_SEEK)         == 0
                                                          && m_messenger.GetPacketCount(CDVDMsg::PLAYER_SEEK_CHAPTER) == 0)
      {
        g_infoManager.SetDisplayAfterSeek(100000);
        SetCaching(CACHESTATE_FLUSH);

        CDVDMsgPlayerSeekChapter &msg(*((CDVDMsgPlayerSeekChapter*)pMsg));
        double start = DVD_NOPTS_VALUE;

        // This should always be the case.
        if(m_pDemuxer && m_pDemuxer->SeekChapter(msg.GetChapter(), &start))
        {
          FlushBuffers(false, start, true);
          // mark mediatime as invalid
          m_av_clock.OMXMediaTime(0.0);

          m_callback.OnPlayBackSeekChapter(msg.GetChapter());
        }

        g_infoManager.SetDisplayAfterSeek();
      }
      else if (pMsg->IsType(CDVDMsg::DEMUXER_RESET))
      {
          m_CurrentAudio.stream = NULL;
          m_CurrentVideo.stream = NULL;
          m_CurrentSubtitle.stream = NULL;

          // we need to reset the demuxer, probably because the streams have changed
          if(m_pDemuxer)
            m_pDemuxer->Reset();
          if(m_pSubtitleDemuxer)
            m_pSubtitleDemuxer->Reset();
      }
      else if (pMsg->IsType(CDVDMsg::PLAYER_SET_AUDIOSTREAM))
      {
        CDVDMsgPlayerSetAudioStream* pMsg2 = (CDVDMsgPlayerSetAudioStream*)pMsg;

        OMXSelectionStream& st = m_SelectionStreams.Get(STREAM_AUDIO, pMsg2->GetStreamId());
        if(st.source != STREAM_SOURCE_NONE)
        {
          if(st.source == STREAM_SOURCE_NAV && m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
          {
            CDVDInputStreamNavigator* pStream = (CDVDInputStreamNavigator*)m_pInputStream;
            if(pStream->SetActiveAudioStream(st.id))
            {
              m_dvd.iSelectedAudioStream = -1;
              CloseAudioStream(false);
              m_messenger.Put(new CDVDMsgPlayerSeek(GetTime(), true, true, true, true, true));
            }
          }
          else
          {
            CloseAudioStream(false);
            OpenAudioStream(st.id, st.source);
            AdaptForcedSubtitles();
            m_messenger.Put(new CDVDMsgPlayerSeek(GetTime(), true, true, true, true, true));
          }
        }
      }
      else if (pMsg->IsType(CDVDMsg::PLAYER_SET_SUBTITLESTREAM))
      {
        CDVDMsgPlayerSetSubtitleStream* pMsg2 = (CDVDMsgPlayerSetSubtitleStream*)pMsg;

        OMXSelectionStream& st = m_SelectionStreams.Get(STREAM_SUBTITLE, pMsg2->GetStreamId());
        if(st.source != STREAM_SOURCE_NONE)
        {
          if(st.source == STREAM_SOURCE_NAV && m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
          {
            CDVDInputStreamNavigator* pStream = (CDVDInputStreamNavigator*)m_pInputStream;
            if(pStream->SetActiveSubtitleStream(st.id))
            {
              m_dvd.iSelectedSPUStream = -1;
              CloseSubtitleStream(false);
            }
          }
          else
          {
            CloseSubtitleStream(false);
            OpenSubtitleStream(st.id, st.source);
          }
        }
      }
      else if (pMsg->IsType(CDVDMsg::PLAYER_SET_SUBTITLESTREAM_VISIBLE))
      {
        CDVDMsgBool* pValue = (CDVDMsgBool*)pMsg;
        SetSubtitleVisibleInternal(pValue->m_value);
      }
      else if (pMsg->IsType(CDVDMsg::PLAYER_SET_STATE))
      {
        g_infoManager.SetDisplayAfterSeek(100000);
        SetCaching(CACHESTATE_FLUSH);

        CDVDMsgPlayerSetState* pMsgPlayerSetState = (CDVDMsgPlayerSetState*)pMsg;

        if (CDVDInputStream::IMenus* ptr = dynamic_cast<CDVDInputStream::IMenus*>(m_pInputStream))
        {
          if(ptr->SetState(pMsgPlayerSetState->GetState()))
          {
            m_dvd.state = DVDSTATE_NORMAL;
            m_dvd.iDVDStillStartTime = 0;
            m_dvd.iDVDStillTime = 0;
          }
        }

        g_infoManager.SetDisplayAfterSeek();
      }
      else if (pMsg->IsType(CDVDMsg::PLAYER_SET_RECORD))
      {
        CDVDInputStream::IChannel* input = dynamic_cast<CDVDInputStream::IChannel*>(m_pInputStream);
        if(input)
          input->Record(*(CDVDMsgBool*)pMsg);
      }
      else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH))
      {
        FlushBuffers(false);
      }
      else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
      {
        int speed = static_cast<CDVDMsgInt*>(pMsg)->m_value;

        // correct our current clock, as it would start going wrong otherwise
        if(m_State.timestamp > 0)
        {
          double offset;
          offset  = m_clock.GetAbsoluteClock() - m_State.timestamp;
          offset *= m_playSpeed / DVD_PLAYSPEED_NORMAL;
          if(offset >  1000) offset =  1000;
          if(offset < -1000) offset = -1000;
          m_State.time     += DVD_TIME_TO_MSEC(offset);
          m_State.timestamp =  m_clock.GetAbsoluteClock();
        }

        if (speed != DVD_PLAYSPEED_PAUSE && m_playSpeed != DVD_PLAYSPEED_PAUSE && speed != m_playSpeed)
          m_callback.OnPlayBackSpeedChanged(speed / DVD_PLAYSPEED_NORMAL);

        if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER) && speed != m_playSpeed)
        {
          CDVDInputStreamPVRManager* pvrinputstream = static_cast<CDVDInputStreamPVRManager*>(m_pInputStream);
          pvrinputstream->Pause( speed == 0 );
        }

        // if playspeed is different then DVD_PLAYSPEED_NORMAL or DVD_PLAYSPEED_PAUSE
        // audioplayer, stops outputing audio to audiorender, but still tries to
        // sleep an correct amount for each packet
        // videoplayer just plays faster after the clock speed has been increased
        // 1. disable audio
        // 2. skip frames and adjust their pts or the clock

        // when switching from trickplay to normal, we may not have a full set of reference frames
        // in decoder and we may get corrupt frames out. Seeking to current time will avoid this.
        if ( TP(speed) || TP(m_playSpeed) ||
           ( (speed == DVD_PLAYSPEED_PAUSE || speed == DVD_PLAYSPEED_NORMAL) &&
             (m_playSpeed != DVD_PLAYSPEED_PAUSE && m_playSpeed != DVD_PLAYSPEED_NORMAL) ) )
          m_messenger.Put(new CDVDMsgPlayerSeek(GetTime(), (speed < 0), true, false, false, true));

        m_playSpeed = speed;
        m_caching = CACHESTATE_DONE;
        m_clock.SetSpeed(speed);
        m_av_clock.OMXSetSpeed(speed);
        m_av_clock.OMXPause();
        m_omxPlayerAudio.SetSpeed(speed);
        m_omxPlayerVideo.SetSpeed(speed);

        // We can't pause demuxer until our buffers are full. Doing so will result in continued
        // calls to Read() which may then block indefinitely (CDVDInputStreamRTMP for example).
        if(m_pDemuxer)
        {
          m_DemuxerPausePending = (speed == DVD_PLAYSPEED_PAUSE);
          if (!m_DemuxerPausePending)
            m_pDemuxer->SetSpeed(speed);
        }
        CLog::Log(LOGDEBUG, "COMXPlayer - CDVDMsg::PLAYER_SETSPEED speed : %d", speed);
      }
      else if (pMsg->IsType(CDVDMsg::PLAYER_CHANNEL_SELECT_NUMBER) && m_messenger.GetPacketCount(CDVDMsg::PLAYER_CHANNEL_SELECT_NUMBER) == 0)
      {
        FlushBuffers(false);
        CDVDInputStream::IChannel* input = dynamic_cast<CDVDInputStream::IChannel*>(m_pInputStream);
        if(input && input->SelectChannelByNumber(static_cast<CDVDMsgInt*>(pMsg)->m_value))
        {
          SAFE_DELETE(m_pDemuxer);
        }else
        {
          CLog::Log(LOGWARNING, "%s - failed to switch channel. playback stopped", __FUNCTION__);
          CApplicationMessenger::Get().MediaStop(false);
        }
      }
      else if (pMsg->IsType(CDVDMsg::PLAYER_CHANNEL_SELECT) && m_messenger.GetPacketCount(CDVDMsg::PLAYER_CHANNEL_SELECT) == 0)
      {
        FlushBuffers(false);
        CDVDInputStream::IChannel* input = dynamic_cast<CDVDInputStream::IChannel*>(m_pInputStream);
        if(input && input->SelectChannel(static_cast<CDVDMsgType <CPVRChannel> *>(pMsg)->m_value))
        {
          SAFE_DELETE(m_pDemuxer);
        }else
        {
          CLog::Log(LOGWARNING, "%s - failed to switch channel. playback stopped", __FUNCTION__);
          CApplicationMessenger::Get().MediaStop(false);
        }
      }
      else if (pMsg->IsType(CDVDMsg::PLAYER_CHANNEL_NEXT) || pMsg->IsType(CDVDMsg::PLAYER_CHANNEL_PREV))
      {
        CDVDInputStream::IChannel* input = dynamic_cast<CDVDInputStream::IChannel*>(m_pInputStream);
        if(input)
        {
          bool bSwitchSuccessful(false);
          bool bShowPreview(CSettings::Get().GetInt("pvrplayback.channelentrytimeout") > 0);

          if (!bShowPreview)
          {
            g_infoManager.SetDisplayAfterSeek(100000);
            FlushBuffers(false);
          }

          if(pMsg->IsType(CDVDMsg::PLAYER_CHANNEL_NEXT))
            bSwitchSuccessful = input->NextChannel(bShowPreview);
          else
            bSwitchSuccessful = input->PrevChannel(bShowPreview);

          if(bSwitchSuccessful)
          {
            if (bShowPreview)
            {
              UpdateApplication(0);
              m_ChannelEntryTimeOut.Set(CSettings::Get().GetInt("pvrplayback.channelentrytimeout"));
            }
            else
            {
              m_ChannelEntryTimeOut.SetInfinite();
              SAFE_DELETE(m_pDemuxer);

              g_infoManager.SetDisplayAfterSeek();
            }
          }
          else
          {
            CLog::Log(LOGWARNING, "%s - failed to switch channel. playback stopped", __FUNCTION__);
            CApplicationMessenger::Get().MediaStop(false);
          }
        }
      }
      else if (pMsg->IsType(CDVDMsg::GENERAL_GUI_ACTION))
        OnAction(((CDVDMsgType<CAction>*)pMsg)->m_value);
      else if (pMsg->IsType(CDVDMsg::PLAYER_STARTED))
      {
        int player = ((CDVDMsgInt*)pMsg)->m_value;
        if(player == DVDPLAYER_AUDIO)
          m_CurrentAudio.started = true;
        if(player == DVDPLAYER_VIDEO)
          m_CurrentVideo.started = true;

        if ((player == DVDPLAYER_AUDIO || player == DVDPLAYER_VIDEO) &&
           (TPA(m_playSpeed) || !m_HasAudio || m_CurrentAudio.started) &&
           (!m_HasVideo || m_CurrentVideo.started))
        {
          CLog::Log(LOGDEBUG, "COMXPlayer::HandleMessages - player started RESET");
          m_av_clock.OMXReset(m_HasVideo, m_playSpeed != DVD_PLAYSPEED_NORMAL && m_playSpeed != DVD_PLAYSPEED_PAUSE ? false:m_HasAudio);
        }

        CLog::Log(LOGDEBUG, "COMXPlayer::HandleMessages - player started %d (tpa:%d,a:%d,v:%d)", player, TPA(m_playSpeed), m_CurrentAudio.started, m_CurrentVideo.started);
      }
      else if (pMsg->IsType(CDVDMsg::PLAYER_DISPLAYTIME))
      {
        COMXPlayer::SPlayerState& state = ((CDVDMsgType<COMXPlayer::SPlayerState>*)pMsg)->m_value;

        CSingleLock lock(m_StateSection);
        /* prioritize data from video player, but only accept data        *
         * after it has been started to avoid race conditions after seeks */
        if(m_CurrentVideo.started && !m_omxPlayerVideo.SubmittedEOS())
        {
          if(state.player == DVDPLAYER_VIDEO)
            m_State = state;
        }
        else if(m_CurrentAudio.started)
        {
          if(state.player == DVDPLAYER_AUDIO)
            m_State = state;
        }
      }
    }
    catch (...)
    {
      CLog::Log(LOGERROR, "%s - Exception thrown when handling message", __FUNCTION__);
    }

    pMsg->Release();
  }

}

void COMXPlayer::SetCaching(ECacheState state)
{
  if(state == CACHESTATE_FLUSH)
  {
    double level, delay, offset;
    if(GetCachingTimes(level, delay, offset))
      state = CACHESTATE_FULL;
    else
      state = CACHESTATE_INIT;
  }

  if(m_caching == state)
    return;

  CLog::Log(LOGDEBUG, "COMXPlayer::SetCaching - caching state %d", state);
  if(state == CACHESTATE_FULL
  || state == CACHESTATE_INIT
  || state == CACHESTATE_PVR)
  {
    m_clock.SetSpeed(DVD_PLAYSPEED_PAUSE);
    m_av_clock.OMXPause();
    m_omxPlayerAudio.SetSpeed(DVD_PLAYSPEED_PAUSE);
    m_omxPlayerAudio.SendMessage(new CDVDMsg(CDVDMsg::PLAYER_STARTED), 1);
    m_omxPlayerVideo.SetSpeed(DVD_PLAYSPEED_PAUSE);
    m_omxPlayerVideo.SendMessage(new CDVDMsg(CDVDMsg::PLAYER_STARTED), 1);

    if (state == CACHESTATE_PVR)
      m_pInputStream->ResetScanTimeout((unsigned int) CSettings::Get().GetInt("pvrplayback.scantime") * 1000);
  }

  if(state == CACHESTATE_PLAY
  ||(state == CACHESTATE_DONE && m_caching != CACHESTATE_PLAY))
  {
    m_clock.SetSpeed(m_playSpeed);
    m_omxPlayerAudio.SetSpeed(m_playSpeed);
    m_omxPlayerVideo.SetSpeed(m_playSpeed);
    m_pInputStream->ResetScanTimeout(0);
  }
  m_caching = state;
}

void COMXPlayer::SetPlaySpeed(int speed)
{
  m_messenger.Put(new CDVDMsgInt(CDVDMsg::PLAYER_SETSPEED, speed));
  m_omxPlayerAudio.SetSpeed(speed);
  m_omxPlayerVideo.SetSpeed(speed);
  SynchronizeDemuxer(100);
}

bool COMXPlayer::CanPause()
{
  CSingleLock lock(m_StateSection);
  return m_State.canpause;
}

void COMXPlayer::Pause()
{
  CSingleLock lock(m_StateSection);
  if (!m_State.canpause)
    return;
  lock.Leave();

  if(m_playSpeed != DVD_PLAYSPEED_PAUSE && (m_caching == CACHESTATE_FULL || m_caching == CACHESTATE_PVR))
  {
    SetCaching(CACHESTATE_DONE);
    return;
  }

  // return to normal speed if it was paused before, pause otherwise
  if (m_playSpeed == DVD_PLAYSPEED_PAUSE)
  {
    SetPlaySpeed(DVD_PLAYSPEED_NORMAL);
    m_callback.OnPlayBackResumed();
  }
  else
  {
    SetPlaySpeed(DVD_PLAYSPEED_PAUSE);
    m_callback.OnPlayBackPaused();
  }
}

bool COMXPlayer::IsPaused() const
{
  return m_playSpeed == DVD_PLAYSPEED_PAUSE || m_caching == CACHESTATE_FULL || m_caching == CACHESTATE_PVR;
}

bool COMXPlayer::HasVideo() const
{
  return m_HasVideo;
}

bool COMXPlayer::HasAudio() const
{
  return m_HasAudio;
}

bool COMXPlayer::IsPassthrough() const
{
  return m_omxPlayerAudio.Passthrough();
}

bool COMXPlayer::CanSeek()
{
  CSingleLock lock(m_StateSection);
  return m_State.canseek;
}

void COMXPlayer::Seek(bool bPlus, bool bLargeStep, bool bChapterOverride)
{
  // Single step
  if( m_playSpeed == DVD_PLAYSPEED_PAUSE && bPlus && !bLargeStep)
  {
    m_av_clock.OMXStep();
    return;
  }

  if (!m_State.canseek)
    return;

  if (bLargeStep && bChapterOverride && GetChapter() > 0)
  {
    if (!bPlus)
    {
      SeekChapter(GetChapter() - 1);
      return;
    }
    else if (GetChapter() < GetChapterCount())
    {
      SeekChapter(GetChapter() + 1);
      return;
    }
  }

  int64_t seek;
  if (g_advancedSettings.m_videoUseTimeSeeking && GetTotalTime() > 2000*g_advancedSettings.m_videoTimeSeekForwardBig)
  {
    if (bLargeStep)
      seek = bPlus ? g_advancedSettings.m_videoTimeSeekForwardBig : g_advancedSettings.m_videoTimeSeekBackwardBig;
    else
      seek = bPlus ? g_advancedSettings.m_videoTimeSeekForward : g_advancedSettings.m_videoTimeSeekBackward;
    seek *= 1000;
    seek += GetTime();
  }
  else
  {
    float percent;
    if (bLargeStep)
      percent = bPlus ? g_advancedSettings.m_videoPercentSeekForwardBig : g_advancedSettings.m_videoPercentSeekBackwardBig;
    else
      percent = bPlus ? g_advancedSettings.m_videoPercentSeekForward : g_advancedSettings.m_videoPercentSeekBackward;
    seek = (int64_t)(GetTotalTimeInMsec()*(GetPercentage()+percent)/100);
  }

  bool restore = true;
  if (m_Edl.HasCut())
  {
    /*
     * Alter the standard seek position based on whether any commercial breaks have been
     * automatically skipped.
     */
    const int clock = DVD_TIME_TO_MSEC(m_clock.GetClock());
    /*
     * If a large backwards seek occurs within 10 seconds of the end of the last automated
     * commercial skip, then seek back to the start of the commercial break under the assumption
     * it was flagged incorrectly. 10 seconds grace period is allowed in case the watcher has to
     * fumble around finding the remote. Only happens once per commercial break.
     *
     * Small skip does not trigger this in case the start of the commercial break was in fact fine
     * but it skipped too far into the program. In that case small skip backwards behaves as normal.
     */
    if (!bPlus && bLargeStep
    &&  m_EdlAutoSkipMarkers.seek_to_start
    &&  clock >= m_EdlAutoSkipMarkers.commbreak_end
    &&  clock <= m_EdlAutoSkipMarkers.commbreak_end + 10*1000) // Only if within 10 seconds of the end (in msec)
    {
      CLog::Log(LOGDEBUG, "%s - Seeking back to start of commercial break [%s - %s] as large backwards skip activated within 10 seconds of the automatic commercial skip (only done once per break).",
                __FUNCTION__, CEdl::MillisecondsToTimeString(m_EdlAutoSkipMarkers.commbreak_start).c_str(),
                CEdl::MillisecondsToTimeString(m_EdlAutoSkipMarkers.commbreak_end).c_str());
      seek = m_EdlAutoSkipMarkers.commbreak_start;
      restore = false;
      m_EdlAutoSkipMarkers.seek_to_start = false; // So this will only happen within the 10 second grace period once.
    }
    /*
     * If big skip forward within the last "reverted" commercial break, seek to the end of the
     * commercial break under the assumption that the break was incorrectly flagged and playback has
     * now reached the actual start of the commercial break. Assume that the end is flagged more
     * correctly than the landing point for a standard big skip (ends seem to be flagged more
     * accurately than the start).
     */
    else if (bPlus && bLargeStep
    &&       clock >= m_EdlAutoSkipMarkers.commbreak_start
    &&       clock <= m_EdlAutoSkipMarkers.commbreak_end)
    {
      CLog::Log(LOGDEBUG, "%s - Seeking to end of previously skipped commercial break [%s - %s] as big forwards skip activated within the break.",
                __FUNCTION__, CEdl::MillisecondsToTimeString(m_EdlAutoSkipMarkers.commbreak_start).c_str(),
                CEdl::MillisecondsToTimeString(m_EdlAutoSkipMarkers.commbreak_end).c_str());
      seek = m_EdlAutoSkipMarkers.commbreak_end;
      restore = false;
    }
  }

  int64_t time = GetTime();
  if(g_application.CurrentFileItem().IsStack()
  && (seek > GetTotalTimeInMsec() || seek < 0))
  {
    g_application.SeekTime((seek - time) * 0.001 + g_application.GetTime());
    // warning, don't access any dvdplayer variables here as
    // the dvdplayer object may have been destroyed
    return;
  }

  m_messenger.Put(new CDVDMsgPlayerSeek((int)seek, !bPlus, true, false, restore));
  SynchronizeDemuxer(100);
  if (seek < 0) seek = 0;
  m_callback.OnPlayBackSeek((int)seek, (int)(seek - time));
}

bool COMXPlayer::SeekScene(bool bPlus)
{
  if (!m_Edl.HasSceneMarker())
    return false;

  /*
   * There is a 5 second grace period applied when seeking for scenes backwards. If there is no
   * grace period applied it is impossible to go backwards past a scene marker.
   */
  int64_t clock = GetTime();
  if (!bPlus && clock > 5 * 1000) // 5 seconds
    clock -= 5 * 1000;

  int64_t iScenemarker;
  if (m_Edl.GetNextSceneMarker(bPlus, clock, &iScenemarker))
  {
    /*
     * Seeking is flushed and inaccurate, just like Seek()
     */
    m_messenger.Put(new CDVDMsgPlayerSeek((int)iScenemarker, !bPlus, true, false, false));
    SynchronizeDemuxer(100);
    return true;
  }
  return false;
}

void COMXPlayer::GetAudioInfo(CStdString &strAudioInfo)
{
  { CSingleLock lock(m_StateSection);
    strAudioInfo = StringUtils::Format("D(%s)", m_StateInput.demux_audio.c_str());
  }
  strAudioInfo += StringUtils::Format("\nP(%s)", m_omxPlayerAudio.GetPlayerInfo().c_str());
}

void COMXPlayer::GetVideoInfo(CStdString &strVideoInfo)
{
  { CSingleLock lock(m_StateSection);
    strVideoInfo = StringUtils::Format("D(%s)", m_StateInput.demux_video.c_str());
  }
  strVideoInfo += StringUtils::Format("\nP(%s)", m_omxPlayerVideo.GetPlayerInfo().c_str());
}

void COMXPlayer::GetGeneralInfo(CStdString& strGeneralInfo)
{
  if (!m_bStop)
  {
    double apts = m_omxPlayerAudio.GetCurrentPts();
    double vpts = m_omxPlayerVideo.GetCurrentPts();
    double dDiff = 0;

    if( apts != DVD_NOPTS_VALUE && vpts != DVD_NOPTS_VALUE )
      dDiff = (apts - vpts) / DVD_TIME_BASE;

    CStdString strEDL = StringUtils::Format(", edl:%s", m_Edl.GetInfo().c_str());

    CStdString strBuf;
    CSingleLock lock(m_StateSection);
    if(m_StateInput.cache_bytes >= 0)
    {
      strBuf += StringUtils::Format(" cache:%s %2.0f%%"
                         , StringUtils::SizeToString(m_State.cache_bytes).c_str()
                         , m_State.cache_level * 100);
      if(m_playSpeed == 0 || m_caching == CACHESTATE_FULL)
        strBuf += StringUtils::Format(" %d sec", DVD_TIME_TO_SEC(m_State.cache_delay));
    }

    strGeneralInfo = StringUtils::Format("C( ad:% 6.3f a/v:% 6.3f%s, dcpu:%2i%% acpu:%2i%% vcpu:%2i%%%s af:%d%% vf:%d%% amp:% 5.2f )"
                         , m_omxPlayerAudio.GetDelay()
                         , dDiff
                         , strEDL.c_str()
                         , (int)(CThread::GetRelativeUsage()*100)
                         , (int)(m_omxPlayerAudio.GetRelativeUsage()*100)
                         , (int)(m_omxPlayerVideo.GetRelativeUsage()*100)
                         , strBuf.c_str()
                         , m_audio_fifo
                         , m_video_fifo
                         , m_omxPlayerAudio.GetDynamicRangeAmplification());

  }
}

void COMXPlayer::SeekPercentage(float iPercent)
{
  int64_t iTotalTime = GetTotalTimeInMsec();

  if (!iTotalTime)
    return;

  SeekTime((int64_t)(iTotalTime * iPercent / 100));
}

float COMXPlayer::GetPercentage()
{
  int64_t iTotalTime = GetTotalTimeInMsec();

  if (!iTotalTime)
    return 0.0f;

  return GetTime() * 100 / (float)iTotalTime;
}

float COMXPlayer::GetCachePercentage()
{
  CSingleLock lock(m_StateSection);
  return m_StateInput.cache_offset * 100; // NOTE: Percentage returned is relative
}

void COMXPlayer::SetAVDelay(float fValue)
{
  m_omxPlayerVideo.SetDelay(fValue * DVD_TIME_BASE);
}

float COMXPlayer::GetAVDelay()
{
  return m_omxPlayerVideo.GetDelay() / (float)DVD_TIME_BASE;
}

void COMXPlayer::SetSubTitleDelay(float fValue)
{
  m_omxPlayerVideo.SetSubtitleDelay(-fValue * DVD_TIME_BASE);
}

float COMXPlayer::GetSubTitleDelay()
{
  return -m_omxPlayerVideo.GetSubtitleDelay() / DVD_TIME_BASE;
}

// priority: 1: libdvdnav, 2: external subtitles, 3: muxed subtitles
int COMXPlayer::GetSubtitleCount()
{
  return m_SelectionStreams.Count(STREAM_SUBTITLE);
}

int COMXPlayer::GetSubtitle()
{
  return m_SelectionStreams.IndexOf(STREAM_SUBTITLE, *this);
}

void COMXPlayer::GetSubtitleStreamInfo(int index, SPlayerSubtitleStreamInfo &info)
{
  if (index < 0 || index > (int) GetSubtitleCount() - 1)
    return;

  OMXSelectionStream& s = m_SelectionStreams.Get(STREAM_SUBTITLE, index);
  if(s.name.length() > 0)
    info.name = s.name;

  if(s.type == STREAM_NONE)
    info.name += "(Invalid)";

  info.language = s.language;
}

void COMXPlayer::SetSubtitle(int iStream)
{
  CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleStream = iStream;
  m_messenger.Put(new CDVDMsgPlayerSetSubtitleStream(iStream));
}

bool COMXPlayer::GetSubtitleVisible()
{
  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
  {
    CDVDInputStreamNavigator* pStream = (CDVDInputStreamNavigator*)m_pInputStream;
    if(pStream->IsInMenu())
      return CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleOn;
    else
      return pStream->IsSubtitleStreamEnabled();
  }

  return m_omxPlayerVideo.IsSubtitleEnabled();
}

void COMXPlayer::SetSubtitleVisible(bool bVisible)
{
  CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleOn = bVisible;
  m_messenger.Put(new CDVDMsgBool(CDVDMsg::PLAYER_SET_SUBTITLESTREAM_VISIBLE, bVisible));
}

void COMXPlayer::SetSubtitleVisibleInternal(bool bVisible)
{
  CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleOn = bVisible;
  m_omxPlayerVideo.EnableSubtitle(bVisible);

  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
    static_cast<CDVDInputStreamNavigator*>(m_pInputStream)->EnableSubtitleStream(bVisible);
}

int COMXPlayer::GetAudioStreamCount()
{
  return m_SelectionStreams.Count(STREAM_AUDIO);
}

int COMXPlayer::GetAudioStream()
{
  return m_SelectionStreams.IndexOf(STREAM_AUDIO, *this);
}

void COMXPlayer::SetAudioStream(int iStream)
{
  CMediaSettings::Get().GetCurrentVideoSettings().m_AudioStream = iStream;
  m_messenger.Put(new CDVDMsgPlayerSetAudioStream(iStream));
  SynchronizeDemuxer(100);
}

TextCacheStruct_t* COMXPlayer::GetTeletextCache()
{
  if (m_CurrentTeletext.id < 0)
    return 0;

  return m_dvdPlayerTeletext.GetTeletextCache();
}

void COMXPlayer::LoadPage(int p, int sp, unsigned char* buffer)
{
  if (m_CurrentTeletext.id < 0)
      return;

  return m_dvdPlayerTeletext.LoadPage(p, sp, buffer);
}

void COMXPlayer::SeekTime(int64_t iTime)
{
  int seekOffset = (int)(iTime - GetTime());
  m_messenger.Put(new CDVDMsgPlayerSeek((int)iTime, true, true, true));
  SynchronizeDemuxer(100);
  m_callback.OnPlayBackSeek((int)iTime, seekOffset);
}

// return the time in milliseconds
int64_t COMXPlayer::GetTime()
{
  CSingleLock lock(m_StateSection);
  double offset = 0;
  const double limit  = DVD_MSEC_TO_TIME(200);
  if(m_State.timestamp > 0)
  {
    offset  = m_clock.GetAbsoluteClock() - m_State.timestamp;
    offset *= m_playSpeed / DVD_PLAYSPEED_NORMAL;
    if(offset >  limit) offset =  limit;
    if(offset < -limit) offset = -limit;
  }
  //{CLog::Log(LOGINFO, "%s: time:%.2f stamp:%.2f dts:%d m:%d (p:%d,c:%d) =%llu", __func__, (double)m_State.time, (double)m_State.timestamp, (int)DVD_TIME_TO_MSEC(m_State.dts + m_offset_pts), (int)DVD_TIME_TO_MSEC(m_stamp), (int)m_playSpeed, (int)m_caching, llrint(m_State.time + DVD_TIME_TO_MSEC(offset)));}
  return llrint(m_State.time + DVD_TIME_TO_MSEC(offset));
}

// return length in msec
int64_t COMXPlayer::GetTotalTimeInMsec()
{
  CSingleLock lock(m_StateSection);
  return llrint(m_State.time_total);
}

// return length in seconds.. this should be changed to return in milleseconds throughout xbmc
int64_t COMXPlayer::GetTotalTime()
{
  return GetTotalTimeInMsec();
}

void COMXPlayer::ToFFRW(int iSpeed)
{
  // can't rewind in menu as seeking isn't possible
  // forward is fine
  if (iSpeed < 0 && IsInMenu()) return;
  SetPlaySpeed(iSpeed * DVD_PLAYSPEED_NORMAL);
}

bool COMXPlayer::OpenAudioStream(int iStream, int source, bool reset)
{
  CLog::Log(LOGNOTICE, "Opening audio stream: %i source: %i", iStream, source);

  if (!m_pDemuxer)
  {
    CLog::Log(LOGWARNING, "Opening audio stream: no demuxer");
    return false;
  }

  CDemuxStream* pStream = m_pDemuxer->GetStream(iStream);
  if (!pStream || pStream->disabled)
  {
    CLog::Log(LOGWARNING, "Opening audio stream: pStream=%p disabled=%d", pStream, pStream ? pStream->disabled:0);
    return false;
  }

  if( m_CurrentAudio.id < 0 &&  m_CurrentVideo.id >= 0 )
  {
    // up until now we wheren't playing audio, but we did play video
    // this will change what is used to sync the dvdclock.
    // since the new audio data doesn't have to have any relation
    // to the current video data in the packet que, we have to
    // wait for it to empty

    // this happens if a new cell has audio data, but previous didn't
    // and both have video data

    SynchronizePlayers(SYNCSOURCE_AUDIO);
  }

  CDVDStreamInfo hint(*pStream, true);

  if(m_CurrentAudio.id    < 0
  || m_CurrentAudio.hint != hint)
  {
    if(!m_omxPlayerAudio.OpenStream(hint))
    {
      /* mark stream as disabled, to disallaw further attempts*/
      CLog::Log(LOGWARNING, "%s - Unsupported stream %d. Stream disabled.", __FUNCTION__, iStream);
      pStream->disabled = true;
      pStream->SetDiscard(AVDISCARD_ALL);
      return false;
    }
  }
  else if (reset)
    m_omxPlayerAudio.SendMessage(new CDVDMsg(CDVDMsg::GENERAL_RESET));

  /* store information about stream */
  m_CurrentAudio.id = iStream;
  m_CurrentAudio.source = source;
  m_CurrentAudio.hint = hint;
  m_CurrentAudio.stream = (void*)pStream;
  m_CurrentAudio.started = false;
  m_HasAudio = true;

  /* we are potentially going to be waiting on this */
  m_omxPlayerAudio.SendMessage(new CDVDMsg(CDVDMsg::PLAYER_STARTED), 1);

  /* software decoding normaly consumes full cpu time so prio it */
  m_omxPlayerAudio.SetPriority(GetPriority()+1);
  CMediaSettings::Get().GetCurrentVideoSettings().m_AudioStream = GetAudioStream();
  return true;
}

bool COMXPlayer::OpenVideoStream(int iStream, int source, bool reset)
{
  CLog::Log(LOGNOTICE, "Opening video stream: %i source: %i", iStream, source);

  if (!m_pDemuxer)
  {
    CLog::Log(LOGWARNING, "Opening video stream: no demuxer");
    return false;
  }

  CDemuxStream* pStream = m_pDemuxer->GetStream(iStream);
  if(!pStream || pStream->disabled)
  {
    CLog::Log(LOGWARNING, "Opening video stream: pStream=%p disabled=%d", pStream, pStream ? pStream->disabled:0);
    return false;
  }
  pStream->SetDiscard(AVDISCARD_NONE);

  CDVDStreamInfo hint(*pStream, true);

  if( m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD) )
  {
    /* set aspect ratio as requested by navigator for dvd's */
    float aspect = static_cast<CDVDInputStreamNavigator*>(m_pInputStream)->GetVideoAspectRatio();
    if(aspect != 0.0)
    {
      hint.aspect = aspect;
      hint.forced_aspect = true;
    }
    hint.software = true;
  }

  boost::shared_ptr<CPVRClient> client;
  if(m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER) &&
     pStream->type == STREAM_VIDEO &&
     g_PVRClients->GetPlayingClient(client) && client->HandlesDemuxing())
  {
    // set the fps in hints
    const CDemuxStreamVideo *stream = static_cast<const CDemuxStreamVideo*>(pStream);
    hint.fpsrate  = stream->iFpsRate;
    hint.fpsscale = stream->iFpsScale;
  }

  CDVDInputStream::IMenus* pMenus = dynamic_cast<CDVDInputStream::IMenus*>(m_pInputStream);
  if(pMenus && pMenus->IsInMenu())
    hint.stills = true;

  if (hint.stereo_mode.empty())
    hint.stereo_mode = CStereoscopicsManager::Get().DetectStereoModeByString(m_filename);

  if(m_CurrentVideo.id    < 0
  || m_CurrentVideo.hint != hint)
  {
    // discard if it's a picture attachment (e.g. album art embedded in MP3 or AAC)
    if ((pStream->flags & AV_DISPOSITION_ATTACHED_PIC) || !m_omxPlayerVideo.OpenStream(hint))
    {
      /* mark stream as disabled, to disallaw further attempts */
      CLog::Log(LOGWARNING, "%s - Unsupported stream %d. Stream disabled.", __FUNCTION__, iStream);
      pStream->disabled = true;
      pStream->SetDiscard(AVDISCARD_ALL);
      return false;
    }
  }
  else if (reset)
    m_omxPlayerVideo.SendMessage(new CDVDMsg(CDVDMsg::GENERAL_RESET));

  /* store information about stream */
  m_CurrentVideo.id = iStream;
  m_CurrentVideo.source = source;
  m_CurrentVideo.hint = hint;
  m_CurrentVideo.stream = (void*)pStream;
  m_CurrentVideo.started = false;
  m_HasVideo = true;

  /* we are potentially going to be waiting on this */
  m_omxPlayerVideo.SendMessage(new CDVDMsg(CDVDMsg::PLAYER_STARTED), 1);

  /* use same priority for video thread as demuxing thread, as */
  /* otherwise demuxer will starve if video consumes the full cpu */
  m_omxPlayerVideo.SetPriority(GetPriority());

  return true;
}

bool COMXPlayer::OpenSubtitleStream(int iStream, int source)
{
  CLog::Log(LOGNOTICE, "Opening Subtitle stream: %i source: %i", iStream, source);

  CDemuxStream* pStream = NULL;
  std::string filename;
  CDVDStreamInfo hint;

  if(STREAM_SOURCE_MASK(source) == STREAM_SOURCE_DEMUX_SUB)
  {
    int index = m_SelectionStreams.IndexOf(STREAM_SUBTITLE, source, iStream);
    if(index < 0)
      return false;
    OMXSelectionStream st = m_SelectionStreams.Get(STREAM_SUBTITLE, index);

    if(!m_pSubtitleDemuxer || m_pSubtitleDemuxer->GetFileName() != st.filename)
    {
      CLog::Log(LOGNOTICE, "Opening Subtitle file: %s", st.filename.c_str());
      auto_ptr<CDVDDemuxVobsub> demux(new CDVDDemuxVobsub());
      if(!demux->Open(st.filename, st.filename2))
        return false;
      m_pSubtitleDemuxer = demux.release();
    }

    pStream = m_pSubtitleDemuxer->GetStream(iStream);
    if(!pStream || pStream->disabled)
      return false;
    pStream->SetDiscard(AVDISCARD_NONE);
    double pts = m_omxPlayerVideo.GetCurrentPts();
    if(pts == DVD_NOPTS_VALUE)
      pts = m_CurrentVideo.dts;
    if(pts == DVD_NOPTS_VALUE)
      pts = 0;
    pts += m_offset_pts;
    m_pSubtitleDemuxer->SeekTime((int)(1000.0 * pts / (double)DVD_TIME_BASE));

    hint.Assign(*pStream, true);
  }
  else if(STREAM_SOURCE_MASK(source) == STREAM_SOURCE_TEXT)
  {
    int index = m_SelectionStreams.IndexOf(STREAM_SUBTITLE, source, iStream);
    if(index < 0)
      return false;
    filename = m_SelectionStreams.Get(STREAM_SUBTITLE, index).filename;

    hint.Clear();
    hint.fpsscale = m_CurrentVideo.hint.fpsscale;
    hint.fpsrate  = m_CurrentVideo.hint.fpsrate;
  }
  else
  {
    if(!m_pDemuxer)
      return false;
    pStream = m_pDemuxer->GetStream(iStream);
    if(!pStream || pStream->disabled)
      return false;
    pStream->SetDiscard(AVDISCARD_NONE);

    hint.Assign(*pStream, true);

    if(m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
      filename = "dvd";
  }

  if(m_CurrentSubtitle.id    < 0
  || m_CurrentSubtitle.hint != hint)
  {
    if(m_CurrentSubtitle.id >= 0)
    {
      CLog::Log(LOGDEBUG, " - codecs hints have changed, must close previous stream");
      CloseSubtitleStream(false);
    }

    if(!m_dvdPlayerSubtitle.OpenStream(hint, filename))
    {
      CLog::Log(LOGWARNING, "%s - Unsupported stream %d. Stream disabled.", __FUNCTION__, iStream);
      if(pStream)
      {
        pStream->disabled = true;
        pStream->SetDiscard(AVDISCARD_ALL);
      }
      return false;
    }
  }
  else
    m_dvdPlayerSubtitle.SendMessage(new CDVDMsg(CDVDMsg::GENERAL_RESET));

  m_CurrentSubtitle.id     = iStream;
  m_CurrentSubtitle.source = source;
  m_CurrentSubtitle.hint   = hint;
  m_CurrentSubtitle.stream = (void*)pStream;
  m_CurrentSubtitle.started = false;

  CMediaSettings::Get().GetCurrentVideoSettings().m_SubtitleStream = GetSubtitle();
  return true;
}

bool COMXPlayer::AdaptForcedSubtitles()
{
  bool valid = false;
  OMXSelectionStream ss = m_SelectionStreams.Get(STREAM_SUBTITLE, GetSubtitle());
  if (ss.flags & CDemuxStream::FLAG_FORCED || !GetSubtitleVisible())
  {
    OMXSelectionStream as = m_SelectionStreams.Get(STREAM_AUDIO, GetAudioStream());
    OMXSelectionStreams streams = m_SelectionStreams.Get(STREAM_SUBTITLE);

    for(OMXSelectionStreams::iterator it = streams.begin(); it != streams.end() && !valid; ++it)
    {
      if (it->flags & CDemuxStream::FLAG_FORCED && g_LangCodeExpander.CompareLangCodes(it->language, as.language))
      {
        if(OpenSubtitleStream(it->id, it->source))
        {
          valid = true;
          SetSubtitleVisibleInternal(true);
        }
      }
    }
    if(!valid)
    {
      CloseSubtitleStream(true);
      SetSubtitleVisibleInternal(false);
    }
  }
  return valid;
}

bool COMXPlayer::OpenTeletextStream(int iStream, int source)
{
  if (!m_pDemuxer)
    return false;

  CDemuxStream* pStream = m_pDemuxer->GetStream(iStream);
  if(!pStream || pStream->disabled)
    return false;

  CDVDStreamInfo hint(*pStream, true);

  if (!m_dvdPlayerTeletext.CheckStream(hint))
    return false;

  CLog::Log(LOGNOTICE, "Opening teletext stream: %i source: %i", iStream, source);

  if(m_CurrentTeletext.id    < 0
  || m_CurrentTeletext.hint != hint)
  {
    if(m_CurrentTeletext.id >= 0)
    {
      CLog::Log(LOGDEBUG, " - teletext codecs hints have changed, must close previous stream");
      CloseTeletextStream(true);
    }

    if (!m_dvdPlayerTeletext.OpenStream(hint))
    {
      /* mark stream as disabled, to disallaw further attempts*/
      CLog::Log(LOGWARNING, "%s - Unsupported teletext stream %d. Stream disabled.", __FUNCTION__, iStream);
      pStream->disabled = true;
      pStream->SetDiscard(AVDISCARD_ALL);
      return false;
    }
  }
  else
    m_dvdPlayerTeletext.SendMessage(new CDVDMsg(CDVDMsg::GENERAL_RESET));

  /* store information about stream */
  m_CurrentTeletext.id      = iStream;
  m_CurrentTeletext.source  = source;
  m_CurrentTeletext.hint    = hint;
  m_CurrentTeletext.stream  = (void*)pStream;
  m_CurrentTeletext.started = false;

  return true;
}

bool COMXPlayer::CloseAudioStream(bool bWaitForBuffers)
{
  if (m_CurrentAudio.id < 0)
    return false;

  CLog::Log(LOGNOTICE, "Closing audio stream");

  if(bWaitForBuffers)
    SetCaching(CACHESTATE_DONE);

  m_omxPlayerAudio.CloseStream(bWaitForBuffers);

  m_CurrentAudio.Clear();
  return true;
}

bool COMXPlayer::CloseVideoStream(bool bWaitForBuffers)
{
  if (m_CurrentVideo.id < 0)
    return false;

  CLog::Log(LOGNOTICE, "Closing video stream");

  if(bWaitForBuffers)
    SetCaching(CACHESTATE_DONE);

  m_omxPlayerVideo.CloseStream(bWaitForBuffers);

  m_CurrentVideo.Clear();
  return true;
}

bool COMXPlayer::CloseSubtitleStream(bool bKeepOverlays)
{
  if (m_CurrentSubtitle.id < 0)
    return false;

  CLog::Log(LOGNOTICE, "Closing subtitle stream");

  m_dvdPlayerSubtitle.CloseStream(!bKeepOverlays);

  m_CurrentSubtitle.Clear();
  return true;
}

bool COMXPlayer::CloseTeletextStream(bool bWaitForBuffers)
{
  if (m_CurrentTeletext.id < 0)
    return false;

  CLog::Log(LOGNOTICE, "Closing teletext stream");

  if(bWaitForBuffers)
    SetCaching(CACHESTATE_DONE);

  m_dvdPlayerTeletext.CloseStream(bWaitForBuffers);

  m_CurrentTeletext.Clear();
  return true;
}

void COMXPlayer::FlushBuffers(bool queued, double pts, bool accurate)
{
  double startpts;

  CLog::Log(LOGNOTICE, "FlushBuffers: q:%d pts:%.0f a:%d", queued, pts, accurate);

  if (!TP(m_playSpeed))
    m_av_clock.OMXStop();
  m_av_clock.OMXPause();
  m_stepped           = false;

  /* for now, ignore accurate flag as it discards keyframes and causes corrupt frames */
  if(0 && accurate)
    startpts = pts;
  else
    startpts = DVD_NOPTS_VALUE;

  /* call with demuxer pts */
  if(startpts != DVD_NOPTS_VALUE)
    startpts -= m_offset_pts;

  m_CurrentAudio.inited      = false;
  m_CurrentAudio.dts         = DVD_NOPTS_VALUE;
  m_CurrentAudio.startpts    = startpts;

  m_CurrentVideo.inited      = false;
  m_CurrentVideo.dts         = DVD_NOPTS_VALUE;
  m_CurrentVideo.startpts    = startpts;

  m_CurrentSubtitle.inited   = false;
  m_CurrentSubtitle.dts      = DVD_NOPTS_VALUE;
  m_CurrentSubtitle.startpts = startpts;

  m_CurrentTeletext.inited   = false;
  m_CurrentTeletext.dts      = DVD_NOPTS_VALUE;
  m_CurrentTeletext.startpts = startpts;

  if(queued)
  {
    m_omxPlayerAudio.SendMessage(new CDVDMsg(CDVDMsg::GENERAL_RESET));
    m_omxPlayerVideo.SendMessage(new CDVDMsg(CDVDMsg::GENERAL_RESET));
    m_omxPlayerVideo.SendMessage(new CDVDMsg(CDVDMsg::VIDEO_NOSKIP));
    m_dvdPlayerSubtitle.SendMessage(new CDVDMsg(CDVDMsg::GENERAL_RESET));
    m_dvdPlayerTeletext.SendMessage(new CDVDMsg(CDVDMsg::GENERAL_RESET));
    SynchronizePlayers(SYNCSOURCE_ALL);
  }
  else
  {
    m_omxPlayerAudio.Flush();
    m_omxPlayerVideo.Flush();
    m_dvdPlayerSubtitle.Flush();
    m_dvdPlayerTeletext.Flush();

    // clear subtitle and menu overlays
    m_overlayContainer.Clear();

    if(m_playSpeed == DVD_PLAYSPEED_NORMAL
    || m_playSpeed == DVD_PLAYSPEED_PAUSE)
    {
      // make sure players are properly flushed, should put them in stalled state
      CDVDMsgGeneralSynchronize* msg = new CDVDMsgGeneralSynchronize(1000, 0);
      m_omxPlayerAudio.SendMessage(msg->Acquire(), 1);
      m_omxPlayerVideo.SendMessage(msg->Acquire(), 1);
      msg->Wait(&m_bStop, 0);
      msg->Release();

      // purge any pending PLAYER_STARTED messages
      m_messenger.Flush(CDVDMsg::PLAYER_STARTED);

      // we should now wait for init cache
      SetCaching(CACHESTATE_FLUSH);
      m_CurrentAudio.started    = false;
      m_CurrentVideo.started    = false;
      m_CurrentSubtitle.started = false;
      m_CurrentTeletext.started = false;
    }

    if(pts != DVD_NOPTS_VALUE)
      m_clock.Discontinuity(pts);
    UpdatePlayState(0);

    // update state, buffers are flushed and it may take some time until
    // we get an update from players
    CSingleLock lock(m_StateSection);
    m_State = m_StateInput;
  }
}

// since we call ffmpeg functions to decode, this is being called in the same thread as ::Process() is
int COMXPlayer::OnDVDNavResult(void* pData, int iMessage)
{
  if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY))
  {
    if(iMessage == 0)
      m_overlayContainer.Add((CDVDOverlay*)pData);
    else if(iMessage == 1)
      m_messenger.Put(new CDVDMsg(CDVDMsg::GENERAL_FLUSH));
    else if(iMessage == 2)
      m_dvd.iSelectedAudioStream = *(int*)pData;
    else if(iMessage == 3)
      m_dvd.iSelectedSPUStream   = *(int*)pData;
    else if(iMessage == 4)
      m_omxPlayerVideo.EnableSubtitle(*(int*)pData ? true: false);
    else if(iMessage == 5)
    {
      if (m_dvd.state != DVDSTATE_STILL)
      {
        // else notify the player we have received a still frame

        m_dvd.iDVDStillTime      = *(int*)pData;
        m_dvd.iDVDStillStartTime = XbmcThreads::SystemClockMillis();

        /* adjust for the output delay in the video queue */
        unsigned int time = 0;
        if( m_CurrentVideo.stream && m_dvd.iDVDStillTime > 0 )
        {
          time = (unsigned int)(m_omxPlayerVideo.GetOutputDelay() / ( DVD_TIME_BASE / 1000 ));
          if( time < 10000 && time > 0 )
            m_dvd.iDVDStillTime += time;
        }
        m_dvd.state = DVDSTATE_STILL;
        CLog::Log(LOGDEBUG,
                  "DVDNAV_STILL_FRAME - waiting %i sec, with delay of %d sec",
                  m_dvd.iDVDStillTime, time / 1000);
      }
    }
    else if (iMessage == 6)
    {
      m_dvd.state = DVDSTATE_NORMAL;
      CLog::Log(LOGDEBUG, "COMXPlayer::OnDVDNavResult - libbluray read error (DVDSTATE_NORMAL)");
      CGUIDialogKaiToast::QueueNotification(g_localizeStrings.Get(25008), g_localizeStrings.Get(25009));
    }

    return 0;
  }

  if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
  {
    CDVDInputStreamNavigator* pStream = (CDVDInputStreamNavigator*)m_pInputStream;

    switch (iMessage)
    {
    case DVDNAV_STILL_FRAME:
      {
        //CLog::Log(LOGDEBUG, "DVDNAV_STILL_FRAME");

        dvdnav_still_event_t *still_event = (dvdnav_still_event_t *)pData;
        // should wait the specified time here while we let the player running
        // after that call dvdnav_still_skip(m_dvdnav);

        if (m_dvd.state != DVDSTATE_STILL)
        {
          // else notify the player we have received a still frame

          if(still_event->length < 0xff)
            m_dvd.iDVDStillTime = still_event->length * 1000;
          else
            m_dvd.iDVDStillTime = 0;

          m_dvd.iDVDStillStartTime = XbmcThreads::SystemClockMillis();

          /* adjust for the output delay in the video queue */
          unsigned int time = 0;
          if( m_CurrentVideo.stream && m_dvd.iDVDStillTime > 0 )
          {
            time = (unsigned int)(m_omxPlayerVideo.GetOutputDelay() / ( DVD_TIME_BASE / 1000 ));
            if( time < 10000 && time > 0 )
              m_dvd.iDVDStillTime += time;
          }
          m_dvd.state = DVDSTATE_STILL;
          CLog::Log(LOGDEBUG,
                    "DVDNAV_STILL_FRAME - waiting %i sec, with delay of %d sec",
                    still_event->length, time / 1000);
        }
        return NAVRESULT_HOLD;
      }
      break;
    case DVDNAV_SPU_CLUT_CHANGE:
      {
        m_dvdPlayerSubtitle.SendMessage(new CDVDMsgSubtitleClutChange((uint8_t*)pData));
      }
      break;
    case DVDNAV_SPU_STREAM_CHANGE:
      {
        dvdnav_spu_stream_change_event_t* event = (dvdnav_spu_stream_change_event_t*)pData;

        int iStream = event->physical_wide;
        bool visible = !(iStream & 0x80);

        SetSubtitleVisibleInternal(visible);

        if (iStream >= 0)
          m_dvd.iSelectedSPUStream = (iStream & ~0x80);
        else
          m_dvd.iSelectedSPUStream = -1;

        m_CurrentSubtitle.stream = NULL;
      }
      break;
    case DVDNAV_AUDIO_STREAM_CHANGE:
      {
        // This should be the correct way i think, however we don't have any streams right now
        // since the demuxer hasn't started so it doesn't change. not sure how to do this.
        dvdnav_audio_stream_change_event_t* event = (dvdnav_audio_stream_change_event_t*)pData;

        // Tell system what audiostream should be opened by default
        if (event->logical >= 0)
          m_dvd.iSelectedAudioStream = event->physical;
        else
          m_dvd.iSelectedAudioStream = -1;

        m_CurrentAudio.stream = NULL;
      }
      break;
    case DVDNAV_HIGHLIGHT:
      {
        //dvdnav_highlight_event_t* pInfo = (dvdnav_highlight_event_t*)pData;
        int iButton = pStream->GetCurrentButton();
        CLog::Log(LOGDEBUG, "DVDNAV_HIGHLIGHT: Highlight button %d\n", iButton);
        m_dvdPlayerSubtitle.UpdateOverlayInfo((CDVDInputStreamNavigator*)m_pInputStream, LIBDVDNAV_BUTTON_NORMAL);
      }
      break;
    case DVDNAV_VTS_CHANGE:
      {
        //dvdnav_vts_change_event_t* vts_change_event = (dvdnav_vts_change_event_t*)pData;
        CLog::Log(LOGDEBUG, "DVDNAV_VTS_CHANGE");

        //Make sure we clear all the old overlays here, or else old forced items are left.
        m_overlayContainer.Clear();

        //Force an aspect ratio that is set in the dvdheaders if available
        m_CurrentVideo.hint.aspect = pStream->GetVideoAspectRatio();
        if( m_omxPlayerVideo.IsInited() )
          m_omxPlayerVideo.SendMessage(new CDVDMsgDouble(CDVDMsg::VIDEO_SET_ASPECT, m_CurrentVideo.hint.aspect));

        m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_NAV);
        m_SelectionStreams.Update(m_pInputStream, m_pDemuxer);

        return NAVRESULT_HOLD;
      }
      break;
    case DVDNAV_CELL_CHANGE:
      {
        //dvdnav_cell_change_event_t* cell_change_event = (dvdnav_cell_change_event_t*)pData;
        CLog::Log(LOGDEBUG, "DVDNAV_CELL_CHANGE");

        m_dvd.state = DVDSTATE_NORMAL;

        if( m_omxPlayerVideo.IsInited() )
          m_omxPlayerVideo.SendMessage(new CDVDMsg(CDVDMsg::VIDEO_NOSKIP));
      }
      break;
    case DVDNAV_NAV_PACKET:
      {
          //pci_t* pci = (pci_t*)pData;

          // this should be possible to use to make sure we get
          // seamless transitions over these boundaries
          // if we remember the old vobunits boundaries
          // when a packet comes out of demuxer that has
          // pts values outside that boundary, it belongs
          // to the new vobunit, wich has new timestamps
          UpdatePlayState(0);
      }
      break;
    case DVDNAV_HOP_CHANNEL:
      {
        // This event is issued whenever a non-seamless operation has been executed.
        // Applications with fifos should drop the fifos content to speed up responsiveness.
        CLog::Log(LOGDEBUG, "DVDNAV_HOP_CHANNEL");
        if(m_dvd.state == DVDSTATE_SEEK)
          m_dvd.state = DVDSTATE_NORMAL;
        else
          m_messenger.Put(new CDVDMsg(CDVDMsg::GENERAL_FLUSH));

        return NAVRESULT_ERROR;
      }
      break;
    case DVDNAV_STOP:
      {
        CLog::Log(LOGDEBUG, "DVDNAV_STOP");
        m_dvd.state = DVDSTATE_NORMAL;
        CGUIDialogKaiToast::QueueNotification(g_localizeStrings.Get(16026), g_localizeStrings.Get(16029));
      }
      break;
    default:
    {}
      break;
    }
  }
  return NAVRESULT_NOP;
}

bool COMXPlayer::ShowPVRChannelInfo(void)
{
  bool bReturn(false);

  if (CSettings::Get().GetBool("pvrmenu.infoswitch"))
  {
    int iTimeout = CSettings::Get().GetBool("pvrmenu.infotimeout") ? CSettings::Get().GetInt("pvrmenu.infotime") : 0;
    g_PVRManager.ShowPlayerInfo(iTimeout);

    bReturn = true;
  }

  return bReturn;
}

bool COMXPlayer::OnAction(const CAction &action)
{
#define THREAD_ACTION(action) \
  do { \
    if (!IsCurrentThread()) { \
      m_messenger.Put(new CDVDMsgType<CAction>(CDVDMsg::GENERAL_GUI_ACTION, action)); \
      return true; \
    } \
  } while(false)

  CDVDInputStream::IMenus* pMenus = dynamic_cast<CDVDInputStream::IMenus*>(m_pInputStream);
  if (pMenus)
  {
    if( m_dvd.state == DVDSTATE_STILL && m_dvd.iDVDStillTime != 0 && pMenus->GetTotalButtons() == 0 )
    {
      switch(action.GetID())
      {
        case ACTION_NEXT_ITEM:
        case ACTION_MOVE_RIGHT:
        case ACTION_MOVE_UP:
        case ACTION_SELECT_ITEM:
          {
            THREAD_ACTION(action);
            /* this will force us out of the stillframe */
            CLog::Log(LOGDEBUG, "%s - User asked to exit stillframe", __FUNCTION__);
            m_dvd.iDVDStillStartTime = 0;
            m_dvd.iDVDStillTime = 1;
          }
          return true;
      }
    }


    switch (action.GetID())
    {
/* this code is disabled to allow switching playlist items (dvdimage "stacks") */
#if 0
    case ACTION_PREV_ITEM:  // SKIP-:
      {
        THREAD_ACTION(action);
        CLog::Log(LOGDEBUG, " - pushed prev");
        pMenus->OnPrevious();
        g_infoManager.SetDisplayAfterSeek();
        return true;
      }
      break;
    case ACTION_NEXT_ITEM:  // SKIP+:
      {
        THREAD_ACTION(action);
        CLog::Log(LOGDEBUG, " - pushed next");
        pMenus->OnNext();
        g_infoManager.SetDisplayAfterSeek();
        return true;
      }
      break;
#endif
    case ACTION_SHOW_VIDEOMENU:   // start button
      {
        THREAD_ACTION(action);
        CLog::Log(LOGDEBUG, " - go to menu");
        pMenus->OnMenu();
        if (m_playSpeed == DVD_PLAYSPEED_PAUSE)
        {
          SetPlaySpeed(DVD_PLAYSPEED_NORMAL);
          m_callback.OnPlayBackResumed();
        }
        // send a message to everyone that we've gone to the menu
        CGUIMessage msg(GUI_MSG_VIDEO_MENU_STARTED, 0, 0);
        g_windowManager.SendThreadMessage(msg);
        return true;
      }
      break;
    }

    if (pMenus->IsInMenu())
    {
      switch (action.GetID())
      {
      case ACTION_NEXT_ITEM:
        THREAD_ACTION(action);
        CLog::Log(LOGDEBUG, " - pushed next in menu, stream will decide");
        pMenus->OnNext();
        g_infoManager.SetDisplayAfterSeek();
        return true;
      case ACTION_PREV_ITEM:
        THREAD_ACTION(action);
        CLog::Log(LOGDEBUG, " - pushed prev in menu, stream will decide");
        pMenus->OnPrevious();
        g_infoManager.SetDisplayAfterSeek();
        return true;
      case ACTION_PREVIOUS_MENU:
      case ACTION_NAV_BACK:
        {
          THREAD_ACTION(action);
          CLog::Log(LOGDEBUG, " - menu back");
          pMenus->OnBack();
        }
        break;
      case ACTION_MOVE_LEFT:
        {
          THREAD_ACTION(action);
          CLog::Log(LOGDEBUG, " - move left");
          pMenus->OnLeft();
        }
        break;
      case ACTION_MOVE_RIGHT:
        {
          THREAD_ACTION(action);
          CLog::Log(LOGDEBUG, " - move right");
          pMenus->OnRight();
        }
        break;
      case ACTION_MOVE_UP:
        {
          THREAD_ACTION(action);
          CLog::Log(LOGDEBUG, " - move up");
          pMenus->OnUp();
        }
        break;
      case ACTION_MOVE_DOWN:
        {
          THREAD_ACTION(action);
          CLog::Log(LOGDEBUG, " - move down");
          pMenus->OnDown();
        }
        break;

      case ACTION_MOUSE_MOVE:
      case ACTION_MOUSE_LEFT_CLICK:
        {
          CRect rs, rd;
          g_renderManager.GetVideoRect(rs, rd);
          CPoint pt(action.GetAmount(), action.GetAmount(1));
          if (!rd.PtInRect(pt))
            return false; // out of bounds
          THREAD_ACTION(action);
          // convert to video coords...
          pt -= CPoint(rd.x1, rd.y1);
          pt.x *= rs.Width() / rd.Width();
          pt.y *= rs.Height() / rd.Height();
          pt += CPoint(rs.x1, rs.y1);
          if (action.GetID() == ACTION_MOUSE_LEFT_CLICK)
            return pMenus->OnMouseClick(pt);
          return pMenus->OnMouseMove(pt);
        }
        break;
      case ACTION_SELECT_ITEM:
        {
          THREAD_ACTION(action);
          CLog::Log(LOGDEBUG, " - button select");
          // show button pushed overlay
          if(m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
            m_dvdPlayerSubtitle.UpdateOverlayInfo((CDVDInputStreamNavigator*)m_pInputStream, LIBDVDNAV_BUTTON_CLICKED);

          pMenus->ActivateButton();
        }
        break;
      case REMOTE_0:
      case REMOTE_1:
      case REMOTE_2:
      case REMOTE_3:
      case REMOTE_4:
      case REMOTE_5:
      case REMOTE_6:
      case REMOTE_7:
      case REMOTE_8:
      case REMOTE_9:
        {
          THREAD_ACTION(action);
          // Offset from key codes back to button number
          int button = action.GetID() - REMOTE_0;
          CLog::Log(LOGDEBUG, " - button pressed %d", button);
          pMenus->SelectButton(button);
        }
       break;
      default:
        return false;
        break;
      }
      return true; // message is handled
    }
  }

  if (dynamic_cast<CDVDInputStream::IChannel*>(m_pInputStream))
  {
    switch (action.GetID())
    {
      case ACTION_MOVE_UP:
      case ACTION_NEXT_ITEM:
      case ACTION_CHANNEL_UP:
        m_messenger.Put(new CDVDMsg(CDVDMsg::PLAYER_CHANNEL_NEXT));
        g_infoManager.SetDisplayAfterSeek();
        ShowPVRChannelInfo();
        return true;
      break;

      case ACTION_MOVE_DOWN:
      case ACTION_PREV_ITEM:
      case ACTION_CHANNEL_DOWN:
        m_messenger.Put(new CDVDMsg(CDVDMsg::PLAYER_CHANNEL_PREV));
        g_infoManager.SetDisplayAfterSeek();
        ShowPVRChannelInfo();
        return true;
      break;

      case ACTION_CHANNEL_SWITCH:
      {
        // Offset from key codes back to button number
        int channel = action.GetAmount();
        m_messenger.Put(new CDVDMsgInt(CDVDMsg::PLAYER_CHANNEL_SELECT_NUMBER, channel));
        g_infoManager.SetDisplayAfterSeek();
        ShowPVRChannelInfo();
        return true;
      }
      break;
    }
  }

  switch (action.GetID())
  {
    case ACTION_NEXT_ITEM:
      if (GetChapter() > 0 && GetChapter() < GetChapterCount())
      {
        m_messenger.Put(new CDVDMsgPlayerSeekChapter(GetChapter() + 1));
        g_infoManager.SetDisplayAfterSeek();
        return true;
      }
      else
        break;
    case ACTION_PREV_ITEM:
      if (GetChapter() > 0)
      {
        m_messenger.Put(new CDVDMsgPlayerSeekChapter(GetChapter() - 1));
        g_infoManager.SetDisplayAfterSeek();
        return true;
      }
      else
        break;
  }

  // return false to inform the caller we didn't handle the message
  return false;
}

bool COMXPlayer::IsInMenu() const
{
  CDVDInputStream::IMenus* pStream = dynamic_cast<CDVDInputStream::IMenus*>(m_pInputStream);
  if (pStream)
  {
    if( m_dvd.state == DVDSTATE_STILL )
      return true;
    else
      return pStream->IsInMenu();
  }
  return false;
}

bool COMXPlayer::HasMenu()
{
  CDVDInputStream::IMenus* pStream = dynamic_cast<CDVDInputStream::IMenus*>(m_pInputStream);
  if (pStream)
    return pStream->HasMenu();
  else
    return false;
}

CStdString COMXPlayer::GetPlayerState()
{
  CSingleLock lock(m_StateSection);
  return m_State.player_state;
}

bool COMXPlayer::SetPlayerState(CStdString state)
{
  m_messenger.Put(new CDVDMsgPlayerSetState(state));
  return true;
}

int COMXPlayer::GetChapterCount()
{
  CSingleLock lock(m_StateSection);
  return m_State.chapter_count;
}

int COMXPlayer::GetChapter()
{
  CSingleLock lock(m_StateSection);
  return m_State.chapter;
}

void COMXPlayer::GetChapterName(CStdString& strChapterName)
{
  CSingleLock lock(m_StateSection);
  strChapterName = m_State.chapter_name;
}

int COMXPlayer::SeekChapter(int iChapter)
{
  if (GetChapter() > 0)
  {
    if (iChapter < 0)
      iChapter = 0;
    if (iChapter > GetChapterCount())
      return 0;

    // Seek to the chapter.
    m_messenger.Put(new CDVDMsgPlayerSeekChapter(iChapter));
    SynchronizeDemuxer(100);
  }

  return 0;
}

int COMXPlayer::AddSubtitle(const CStdString& strSubPath)
{
  return AddSubtitleFile(strSubPath);
}

int COMXPlayer::GetCacheLevel() const
{
  CSingleLock lock(m_StateSection);
  return (int)(m_StateInput.cache_level * 100);
}

double COMXPlayer::GetQueueTime()
{
  int a = m_omxPlayerAudio.GetLevel();
  int v = m_omxPlayerVideo.GetLevel();
  return max(a, v) * 8000.0 / 100;
}

void COMXPlayer::GetVideoStreamInfo(SPlayerVideoStreamInfo &info)
{
  info.bitrate = m_omxPlayerVideo.GetVideoBitrate();

  CStdString retVal;
  if (m_pDemuxer && (m_CurrentVideo.id != -1))
  {
    m_pDemuxer->GetStreamCodecName(m_CurrentVideo.id, retVal);
    CDemuxStreamVideo* stream = static_cast<CDemuxStreamVideo*>(m_pDemuxer->GetStream(m_CurrentVideo.id));
    if (stream)
    {
      info.width  = stream->iWidth;
      info.height = stream->iHeight;
    }
  }
  info.videoCodecName = retVal;
  info.videoAspectRatio = g_renderManager.GetAspectRatio();
  g_renderManager.GetVideoRect(info.SrcRect, info.DestRect);
  info.stereoMode = m_omxPlayerVideo.GetStereoMode();
  if (info.stereoMode == "mono")
    info.stereoMode = "";
}

int COMXPlayer::GetSourceBitrate()
{
  if (m_pInputStream)
    return (int)m_pInputStream->GetBitstreamStats().GetBitrate();

  return 0;
}

void COMXPlayer::GetAudioStreamInfo(int index, SPlayerAudioStreamInfo &info)
{
  if (index < 0 || index > GetAudioStreamCount() - 1)
    return;

  if (index == GetAudioStream())
    info.bitrate = m_omxPlayerAudio.GetAudioBitrate();
  else if (m_pDemuxer)
  {
    CDemuxStreamAudio* stream = m_pDemuxer->GetStreamFromAudioId(index);
    if (stream)
      info.bitrate = stream->iBitRate;
  }

  OMXSelectionStream& s = m_SelectionStreams.Get(STREAM_AUDIO, index);
  if(s.language.length() > 0)
    info.language = s.language;

  if(s.name.length() > 0)
    info.name = s.name;

  if(s.type == STREAM_NONE)
    info.name += " (Invalid)";

  if (m_pDemuxer)
  {
    CDemuxStreamAudio* stream = static_cast<CDemuxStreamAudio*>(m_pDemuxer->GetStreamFromAudioId(index));
    if (stream)
    {
      info.channels = stream->iChannels;
      CStdString codecName;
      m_pDemuxer->GetStreamCodecName(stream->iId, codecName);
      info.audioCodecName = codecName;
    }
  }
}

int COMXPlayer::AddSubtitleFile(const std::string& filename, const std::string& subfilename, CDemuxStream::EFlags flags)
{
  std::string ext = URIUtils::GetExtension(filename);
  std::string vobsubfile = subfilename;
  if(ext == ".idx")
  {
    if (vobsubfile.empty())
      vobsubfile = URIUtils::ReplaceExtension(filename, ".sub");

    CDVDDemuxVobsub v;
    if(!v.Open(filename, vobsubfile))
      return -1;
    m_SelectionStreams.Update(NULL, &v);
    int index = m_SelectionStreams.IndexOf(STREAM_SUBTITLE, m_SelectionStreams.Source(STREAM_SOURCE_DEMUX_SUB, filename), 0);
    m_SelectionStreams.Get(STREAM_SUBTITLE, index).flags = flags;
    m_SelectionStreams.Get(STREAM_SUBTITLE, index).filename2 = vobsubfile;
    ExternalStreamInfo info;
    CUtil::GetExternalStreamDetailsFromFilename(m_filename, vobsubfile, info);
    m_SelectionStreams.Get(STREAM_SUBTITLE, index).name = info.name;
    if (m_SelectionStreams.Get(STREAM_SUBTITLE, index).language.empty())
      m_SelectionStreams.Get(STREAM_SUBTITLE, index).language = info.language;

    if (static_cast<CDemuxStream::EFlags>(info.flag) == CDemuxStream::FLAG_NONE)
      m_SelectionStreams.Get(STREAM_SUBTITLE, index).flags = flags;
    else
      m_SelectionStreams.Get(STREAM_SUBTITLE, index).flags = static_cast<CDemuxStream::EFlags>(info.flag);

    return index;
  }
  if(ext == ".sub")
  {
    CStdString strReplace(URIUtils::ReplaceExtension(filename,".idx"));
    if (XFILE::CFile::Exists(strReplace))
      return -1;
  }
  OMXSelectionStream s;
  s.source   = m_SelectionStreams.Source(STREAM_SOURCE_TEXT, filename);
  s.type     = STREAM_SUBTITLE;
  s.id       = 0;
  s.filename = filename;
  ExternalStreamInfo info;
  CUtil::GetExternalStreamDetailsFromFilename(m_filename, filename, info);
  s.name = info.name;
  s.language = info.language;
  if (static_cast<CDemuxStream::EFlags>(info.flag) == CDemuxStream::FLAG_NONE)
    s .flags = flags;
  else
    s.flags = static_cast<CDemuxStream::EFlags>(info.flag);

  m_SelectionStreams.Update(s);
  return m_SelectionStreams.IndexOf(STREAM_SUBTITLE, s.source, s.id);
}

void COMXPlayer::UpdatePlayState(double timeout)
{
  if(m_StateInput.timestamp != 0
  && m_StateInput.timestamp + DVD_MSEC_TO_TIME(timeout) > m_clock.GetAbsoluteClock())
    return;

  SPlayerState state(m_StateInput);

  if     (m_CurrentVideo.dts != DVD_NOPTS_VALUE)
    state.dts = m_CurrentVideo.dts;
  else if(m_CurrentAudio.dts != DVD_NOPTS_VALUE)
    state.dts = m_CurrentAudio.dts;
  else if(m_CurrentVideo.startpts != DVD_NOPTS_VALUE)
    state.dts = m_CurrentVideo.startpts;
  else if(m_CurrentAudio.startpts != DVD_NOPTS_VALUE)
    state.dts = m_CurrentAudio.startpts;

  if(m_pDemuxer)
  {
    state.chapter       = m_pDemuxer->GetChapter();
    state.chapter_count = m_pDemuxer->GetChapterCount();
    m_pDemuxer->GetChapterName(state.chapter_name);

    if(state.dts == DVD_NOPTS_VALUE)
      state.time     = 0;
    else 
      state.time     = DVD_TIME_TO_MSEC(state.dts + m_offset_pts);
    state.time_total = m_pDemuxer->GetStreamLength();
    state.time_src   = ETIMESOURCE_CLOCK;
  }

  state.canpause     = true;
  state.canseek      = true;

  if(m_pInputStream)
  {
    // override from input stream if needed
    CDVDInputStream::IChannel* pChannel = dynamic_cast<CDVDInputStream::IChannel*>(m_pInputStream);
    if (pChannel)
    {
      state.canrecord = pChannel->CanRecord();
      state.recording = pChannel->IsRecording();
    }

    CDVDInputStream::IDisplayTime* pDisplayTime = dynamic_cast<CDVDInputStream::IDisplayTime*>(m_pInputStream);
    if (pDisplayTime && pDisplayTime->GetTotalTime() > 0)
    {
      state.time       = pDisplayTime->GetTime();
      state.time_total = pDisplayTime->GetTotalTime();
      state.time_src   = ETIMESOURCE_INPUT;
    }

    if (CDVDInputStream::IMenus* ptr = dynamic_cast<CDVDInputStream::IMenus*>(m_pInputStream))
    {
      if(!ptr->GetState(state.player_state))
        state.player_state = "";

      if(m_dvd.state == DVDSTATE_STILL)
      {
        state.time       = XbmcThreads::SystemClockMillis() - m_dvd.iDVDStillStartTime;
        state.time_total = m_dvd.iDVDStillTime;
        state.time_src   = ETIMESOURCE_MENU;
      }
    }

    if (CDVDInputStream::ISeekable* ptr = dynamic_cast<CDVDInputStream::ISeekable*>(m_pInputStream))
    {
      state.canpause = ptr->CanPause();
      state.canseek  = ptr->CanSeek();
    }
  }

  if (m_Edl.HasCut())
  {
    state.time        = m_Edl.RemoveCutTime(llrint(state.time));
    state.time_total  = m_Edl.RemoveCutTime(llrint(state.time_total));
  }

  if(state.time_total <= 0)
    state.canseek  = false;

  if (state.time_src == ETIMESOURCE_CLOCK)
    state.time_offset = m_offset_pts;
  else if (state.dts != DVD_NOPTS_VALUE)
    state.time_offset = DVD_MSEC_TO_TIME(state.time) - state.dts;

  if (m_CurrentAudio.id >= 0 && m_pDemuxer)
  {
    CDemuxStream* pStream = m_pDemuxer->GetStream(m_CurrentAudio.id);
    if (pStream && pStream->type == STREAM_AUDIO)
      ((CDemuxStreamAudio*)pStream)->GetStreamInfo(state.demux_audio);
  }
  else
    state.demux_audio = "";

  if (m_CurrentVideo.id >= 0 && m_pDemuxer)
  {
    CDemuxStream* pStream = m_pDemuxer->GetStream(m_CurrentVideo.id);
    if (pStream && pStream->type == STREAM_VIDEO)
      ((CDemuxStreamVideo*)pStream)->GetStreamInfo(state.demux_video);
  }
  else
    state.demux_video = "";

  double level, delay, offset;
  if(GetCachingTimes(level, delay, offset))
  {
    state.cache_delay  = max(0.0, delay);
    state.cache_level  = max(0.0, min(1.0, level));
    state.cache_offset = offset;
  }
  else
  {
    state.cache_delay  = 0.0;
    state.cache_level  = min(1.0, GetQueueTime() / 8000.0);
    state.cache_offset = GetQueueTime() / state.time_total;
  }

  XFILE::SCacheStatus status;
  if(m_pInputStream && m_pInputStream->GetCacheStatus(&status))
  {
    state.cache_bytes = status.forward;
    if(state.time_total)
      state.cache_bytes += m_pInputStream->GetLength() * GetQueueTime() / state.time_total;
  }
  else
    state.cache_bytes = 0;

  state.timestamp = m_clock.GetAbsoluteClock();
  //{CLog::Log(LOGINFO, "%s: time:%.2f stamp:%.2f dts:%d m:%d (p:%d,c:%d) =%llu", __func__, (double)state.time, (double)state.timestamp, (int)DVD_TIME_TO_MSEC(state.dts + m_offset_pts), (int)DVD_TIME_TO_MSEC(m_stamp), (int)m_playSpeed, (int)m_caching, llrint(state.time + DVD_TIME_TO_MSEC(offset)));}

  CSingleLock lock(m_StateSection);
  m_StateInput = state;
}

void COMXPlayer::UpdateApplication(double timeout)
{
  if(m_UpdateApplication != 0
  && m_UpdateApplication + DVD_MSEC_TO_TIME(timeout) > m_clock.GetAbsoluteClock())
    return;

  CDVDInputStream::IChannel* pStream = dynamic_cast<CDVDInputStream::IChannel*>(m_pInputStream);
  if(pStream)
  {
    CFileItem item(g_application.CurrentFileItem());
    if(pStream->UpdateItem(item))
    {
      g_application.CurrentFileItem() = item;
      CApplicationMessenger::Get().SetCurrentItem(item);
    }
  }
  m_UpdateApplication = m_clock.GetAbsoluteClock();
}

bool COMXPlayer::CanRecord()
{
  CSingleLock lock(m_StateSection);
  return m_State.canrecord;
}

bool COMXPlayer::IsRecording()
{
  CSingleLock lock(m_StateSection);
  return m_State.recording;
}

bool COMXPlayer::Record(bool bOnOff)
{
  if (m_pInputStream && (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_TV) ||
                         m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER)) )
  {
    m_messenger.Put(new CDVDMsgBool(CDVDMsg::PLAYER_SET_RECORD, bOnOff));
    return true;
  }
  return false;
}

bool COMXPlayer::GetStreamDetails(CStreamDetails &details)
{
  if (m_pDemuxer)
  {
    std::vector<OMXSelectionStream> subs = m_SelectionStreams.Get(STREAM_SUBTITLE);
    std::vector<CStreamDetailSubtitle> extSubDetails;
    for (unsigned int i = 0; i < subs.size(); i++)
    {
      if (subs[i].filename == m_filename)
        continue;

      CStreamDetailSubtitle p;
      p.m_strLanguage = subs[i].language;
      extSubDetails.push_back(p);
    }

    bool result = CDVDFileInfo::DemuxerToStreamDetails(m_pInputStream, m_pDemuxer, extSubDetails, details);
    if (result && details.GetStreamCount(CStreamDetail::VIDEO) > 0) // this is more correct (dvds in particular)
    {
      /* 
       * We can only obtain the aspect & duration from dvdplayer when the Process() thread is running
       * and UpdatePlayState() has been called at least once. In this case dvdplayer duration/AR will
       * return 0 and we'll have to fallback to the (less accurate) info from the demuxer.
       */
      float aspect = m_omxPlayerVideo.GetAspectRatio();
      if (aspect > 0.0f)
        ((CStreamDetailVideo*)details.GetNthStream(CStreamDetail::VIDEO,0))->m_fAspect = aspect;

      int64_t duration = GetTotalTime() / 1000;
      if (duration > 0)
        ((CStreamDetailVideo*)details.GetNthStream(CStreamDetail::VIDEO,0))->m_iDuration = duration;
    }
    return result;
  }
  else
    return false;
}

CStdString COMXPlayer::GetPlayingTitle()
{
  /* Currently we support only Title Name from Teletext line 30 */
  TextCacheStruct_t* ttcache = m_dvdPlayerTeletext.GetTeletextCache();
  if (ttcache && !ttcache->line30.empty())
    return ttcache->line30;

  return "";
}

bool COMXPlayer::SwitchChannel(const CPVRChannel &channel)
{
  if (!g_PVRManager.CheckParentalLock(channel))
    return false;

  /* set GUI info */
  if (!g_PVRManager.PerformChannelSwitch(channel, true))
    return false;

  UpdateApplication(0);
  UpdatePlayState(0);

  /* make sure the pvr window is updated */
  CGUIWindowPVR *pWindow = (CGUIWindowPVR *) g_windowManager.GetWindow(WINDOW_PVR);
  if (pWindow)
    pWindow->SetInvalid();

  /* select the new channel */
  m_messenger.Put(new CDVDMsgType<CPVRChannel>(CDVDMsg::PLAYER_CHANNEL_SELECT, channel));

  return true;
}

bool COMXPlayer::CachePVRStream(void) const
{
  return m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER) &&
      !g_PVRManager.IsPlayingRecording() &&
      g_advancedSettings.m_bPVRCacheInDvdPlayer;
}

void COMXPlayer::GetRenderFeatures(std::vector<int> &renderFeatures)
{
  renderFeatures.push_back(RENDERFEATURE_STRETCH);
  renderFeatures.push_back(RENDERFEATURE_CROP);
  renderFeatures.push_back(RENDERFEATURE_PIXEL_RATIO);
  renderFeatures.push_back(RENDERFEATURE_ZOOM);
  renderFeatures.push_back(RENDERFEATURE_SHARPNESS);
}

void COMXPlayer::GetDeinterlaceMethods(std::vector<int> &deinterlaceMethods)
{
  deinterlaceMethods.push_back(VS_INTERLACEMETHOD_DEINTERLACE);
}

void COMXPlayer::GetDeinterlaceModes(std::vector<int> &deinterlaceModes)
{
  deinterlaceModes.push_back(VS_DEINTERLACEMODE_AUTO);
  deinterlaceModes.push_back(VS_DEINTERLACEMODE_OFF);
  deinterlaceModes.push_back(VS_DEINTERLACEMODE_FORCE);
}

void COMXPlayer::GetScalingMethods(std::vector<int> &scalingMethods)
{
}

void COMXPlayer::GetAudioCapabilities(std::vector<int> &audioCaps)
{
  audioCaps.push_back(IPC_AUD_OFFSET);
  audioCaps.push_back(IPC_AUD_SELECT_STREAM);
  audioCaps.push_back(IPC_AUD_SELECT_OUTPUT);
  audioCaps.push_back(IPC_AUD_AMP);
}

void COMXPlayer::GetSubtitleCapabilities(std::vector<int> &subCaps)
{
  subCaps.push_back(IPC_SUBS_ALL);
}

#endif
