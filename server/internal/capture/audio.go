package capture

import (
	"fmt"
	"strings"

	"github.com/rs/zerolog"

	"github.com/m1k1o/neko/server/internal/config"
	"github.com/m1k1o/neko/server/pkg/types"
)

// defaultAudioPipeline is the single stereo track used when no audio pipelines are configured.
func defaultAudioPipeline(config *config.Capture) string {
	if config.AudioPipeline != "" {
		// replace {device} with valid device
		return strings.Replace(config.AudioPipeline, "{device}", config.AudioDevice, 1)
	}

	return fmt.Sprintf(
		"pulsesrc device=%s "+
			"! audio/x-raw,channels=2 "+
			"! audioconvert "+
			"! queue max-size-buffers=5 leaky=downstream "+
			"! %s "+
			"! appsink name=appsink", config.AudioDevice, config.AudioCodec.Pipeline,
	)
}

// newAudioSelector creates one lazily started stream sink per configured audio track.
// A pipeline only runs while at least one peer listens to it, so unused tracks cost nothing.
func newAudioSelector(logger zerolog.Logger, config *config.Capture) (*StreamSelectorManagerCtx, []types.AudioTrack) {
	streams := map[string]*StreamSinkManagerCtx{}

	// single legacy track
	if len(config.AudioPipelines) == 0 {
		pipeline := defaultAudioPipeline(config)
		streams["main"] = streamSinkNew(config.AudioCodec, func() (string, error) {
			return pipeline, nil
		}, "audio")

		return streamSelectorNew(config.AudioCodec, streams, []string{"main"}),
			[]types.AudioTrack{{ID: "main", Label: "Default"}}
	}

	tracks := make([]types.AudioTrack, 0, len(config.AudioIDs))
	for _, id := range config.AudioIDs {
		cnf := config.AudioPipelines[id]
		pipeline := cnf.GetPipeline(config.AudioDevice, config.AudioCodec.Pipeline)

		logger.Info().
			Str("audio_id", id).
			Str("pipeline", pipeline).
			Msg("using audio stream pipeline")

		// stream ids must stay unique across audio and video for metrics labels
		streams[id] = streamSinkNew(config.AudioCodec, func() (string, error) {
			return pipeline, nil
		}, "audio_"+id)

		label := cnf.Label
		if label == "" {
			label = id
		}
		tracks = append(tracks, types.AudioTrack{ID: id, Label: label})
	}

	return streamSelectorNew(config.AudioCodec, streams, config.AudioIDs), tracks
}
