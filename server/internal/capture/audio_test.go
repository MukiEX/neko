package capture

import (
	"reflect"
	"strings"
	"testing"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/m1k1o/neko/server/pkg/types"
)

// newTestManager creates a capture manager with its own metrics registry, because
// stream sinks register their metrics globally and only one manager exists in production.
func newTestManager(t *testing.T) *CaptureManagerCtx {
	t.Helper()

	registerer := prometheus.DefaultRegisterer
	prometheus.DefaultRegisterer = prometheus.NewRegistry()
	t.Cleanup(func() { prometheus.DefaultRegisterer = registerer })

	return New(nil, captureConfigFromEnv(t, testVideoPipelines))
}

const testVideoPipelines = `{"main":"ximagesrc display-name={display} ! appsink name=appsink"}`

func audioPipelineOf(t *testing.T, manager *CaptureManagerCtx, id string) string {
	t.Helper()
	stream, ok := manager.AudioSelector().GetStream(types.StreamSelector{ID: id, Type: types.StreamSelectorTypeExact})
	if !ok {
		t.Fatalf("audio stream %q not found", id)
	}
	pipeline, err := stream.(*StreamSinkManagerCtx).pipelineFn()
	if err != nil {
		t.Fatal(err)
	}
	return pipeline
}

func TestAudioTracksDefaultSingleTrack(t *testing.T) {
	manager := newTestManager(t)

	if got, want := manager.AudioTracks(), []types.AudioTrack{{ID: "main", Label: "Default"}}; !reflect.DeepEqual(got, want) {
		t.Fatalf("AudioTracks() = %v, want %v", got, want)
	}
	if got := manager.Audio().ID(); got != "audio" {
		t.Fatalf("default audio stream id = %q, want audio (keeps existing metrics labels)", got)
	}

	pipeline := audioPipelineOf(t, manager, "main")
	if !strings.HasPrefix(pipeline, "pulsesrc device=audio_output.monitor ! audio/x-raw,channels=2") {
		t.Fatalf("default pipeline = %q", pipeline)
	}
}

func TestAudioTracksFromPipelines(t *testing.T) {
	t.Setenv("NEKO_CAPTURE_AUDIO_IDS", "plii,dolbyhp")
	t.Setenv("NEKO_CAPTURE_AUDIO_PIPELINES", `{
		"plii": {"label": "Pro Logic II", "channels": 6, "gst_filter": "audioconvert ! nekosurround mode=plii"},
		"dolbyhp": {"label": "Dolby Headphone", "channels": 6, "gst_filter": "! audioconvert ! nekosurround mode=dolby-headphone"},
		"raw": "pulsesrc device={device} ! opusenc ! appsink name=appsink"
	}`)
	manager := newTestManager(t)

	wantTracks := []types.AudioTrack{{ID: "plii", Label: "Pro Logic II"}, {ID: "dolbyhp", Label: "Dolby Headphone"}}
	if got := manager.AudioTracks(); !reflect.DeepEqual(got, wantTracks) {
		t.Fatalf("AudioTracks() = %v, want %v (ids order, unlisted pipelines left out)", got, wantTracks)
	}
	if got := manager.AudioSelector().IDs(); !reflect.DeepEqual(got, []string{"plii", "dolbyhp"}) {
		t.Fatalf("AudioSelector().IDs() = %v", got)
	}
	if got := manager.Audio().ID(); got != "audio_plii" {
		t.Fatalf("default audio stream = %q, want audio_plii", got)
	}

	plii := audioPipelineOf(t, manager, "plii")
	want := "pulsesrc device=audio_output.monitor ! audio/x-raw,channels=6 ! audioconvert ! nekosurround mode=plii " +
		"! audioconvert ! audio/x-raw,channels=2 ! queue max-size-buffers=5 leaky=downstream ! opusenc inband-fec=true bitrate=128000 ! appsink name=appsink"
	if plii != want {
		t.Fatalf("plii pipeline =\n%q\nwant\n%q", plii, want)
	}

	if dh := audioPipelineOf(t, manager, "dolbyhp"); !strings.Contains(dh, "channels=6 ! audioconvert ! nekosurround mode=dolby-headphone ! audioconvert") {
		t.Fatalf("leading ! in gst_filter not normalized: %q", dh)
	}
}

func TestAudioTracksWithoutIDsUsesAllSorted(t *testing.T) {
	t.Setenv("NEKO_CAPTURE_AUDIO_PIPELINES", `{"b": {"device": "other.monitor"}, "a": "pulsesrc device={device} ! opusenc ! appsink name=appsink"}`)
	manager := newTestManager(t)

	if got := manager.AudioSelector().IDs(); !reflect.DeepEqual(got, []string{"a", "b"}) {
		t.Fatalf("IDs() = %v, want [a b]", got)
	}
	if got := manager.AudioTracks()[1]; got != (types.AudioTrack{ID: "b", Label: "b"}) {
		t.Fatalf("label fallback = %+v", got)
	}
	if got := audioPipelineOf(t, manager, "a"); got != "pulsesrc device=audio_output.monitor ! opusenc ! appsink name=appsink" {
		t.Fatalf("full pipeline with {device} = %q", got)
	}
	if got := audioPipelineOf(t, manager, "b"); !strings.HasPrefix(got, "pulsesrc device=other.monitor ! audio/x-raw,channels=2 ! audioconvert") {
		t.Fatalf("per track device = %q", got)
	}
}
