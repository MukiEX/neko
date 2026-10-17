package webrtc

import (
	"testing"
	"time"

	"github.com/pion/webrtc/v4"
	"github.com/rs/zerolog"

	"github.com/m1k1o/neko/server/pkg/types"
	"github.com/m1k1o/neko/server/pkg/types/codec"
	"github.com/m1k1o/neko/server/pkg/types/event"
)

type fakeAudioStream struct {
	id          string
	subscribers int
}

type fakeAudioSubscription struct {
	stream *fakeAudioStream
}

func (s *fakeAudioStream) ID() string            { return s.id }
func (s *fakeAudioStream) Codec() codec.RTPCodec { return codec.Opus() }
func (s *fakeAudioStream) Bitrate() uint64       { return 0 }
func (s *fakeAudioStream) Subscribe(types.SampleConsumer) (types.StreamSubscription, error) {
	s.subscribers++
	return &fakeAudioSubscription{stream: s}, nil
}

func (s *fakeAudioSubscription) Stream() types.EncodedStream { return s.stream }
func (s *fakeAudioSubscription) Switch(target types.EncodedStream) error {
	s.stream.subscribers--
	s.stream = target.(*fakeAudioStream)
	s.stream.subscribers++
	return nil
}
func (s *fakeAudioSubscription) Close() error {
	s.stream.subscribers--
	return nil
}

type fakeAudioSelector struct {
	ids     []string
	streams map[string]*fakeAudioStream
}

func (s *fakeAudioSelector) IDs() []string         { return s.ids }
func (s *fakeAudioSelector) Codec() codec.RTPCodec { return codec.Opus() }
func (s *fakeAudioSelector) GetStream(sel types.StreamSelector) (types.EncodedStream, bool) {
	stream, ok := s.streams[sel.ID]
	return stream, ok
}

type fakeSession struct {
	types.Session
	sent chan types.PeerAudio
}

func (s *fakeSession) Send(ev string, payload any) {
	if ev == event.SIGNAL_AUDIO {
		s.sent <- payload.(types.PeerAudio)
	}
}

func newAudioTestPeer(t *testing.T) (*WebRTCPeerCtx, *fakeAudioSelector, *fakeSession) {
	t.Helper()

	connection, err := webrtc.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = connection.Close() })

	selector := &fakeAudioSelector{
		ids: []string{"plii", "dolbyhp"},
		streams: map[string]*fakeAudioStream{
			"plii":    {id: "audio_plii"},
			"dolbyhp": {id: "audio_dolbyhp"},
		},
	}

	track, err := NewTrack(zerolog.Nop(), codec.Opus(), connection)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(track.Shutdown)
	track.SetPaused(true)
	if _, err := track.SetStream(selector.streams["plii"]); err != nil {
		t.Fatal(err)
	}

	session := &fakeSession{sent: make(chan types.PeerAudio, 4)}
	peer := &WebRTCPeerCtx{
		logger:        zerolog.Nop(),
		session:       session,
		connection:    connection,
		audio:         selector,
		audioTrack:    track,
		audioDisabled: true,
		audioID:       "plii",
	}
	return peer, selector, session
}

func expectAudioSignal(t *testing.T, session *fakeSession, want types.PeerAudio) {
	t.Helper()
	select {
	case got := <-session.sent:
		if got != want {
			t.Fatalf("signal audio = %+v, want %+v", got, want)
		}
	case <-time.After(time.Second):
		t.Fatalf("no signal audio sent, want %+v", want)
	}
}

func TestPeerSetAudioSwitchesTrack(t *testing.T) {
	peer, selector, session := newAudioTestPeer(t)

	enabled := false
	if err := peer.SetAudio(types.PeerAudioRequest{Disabled: &enabled}); err != nil {
		t.Fatal(err)
	}
	expectAudioSignal(t, session, types.PeerAudio{Disabled: false, ID: "plii"})

	if selector.streams["plii"].subscribers != 1 {
		t.Fatalf("plii subscribers = %d, want 1", selector.streams["plii"].subscribers)
	}

	id := "dolbyhp"
	if err := peer.SetAudio(types.PeerAudioRequest{ID: &id}); err != nil {
		t.Fatal(err)
	}
	expectAudioSignal(t, session, types.PeerAudio{Disabled: false, ID: "dolbyhp"})

	if got := peer.Audio(); got.ID != "dolbyhp" {
		t.Fatalf("Audio().ID = %q, want dolbyhp", got.ID)
	}
	if selector.streams["plii"].subscribers != 0 || selector.streams["dolbyhp"].subscribers != 1 {
		t.Fatalf("subscribers plii=%d dolbyhp=%d, want 0 and 1",
			selector.streams["plii"].subscribers, selector.streams["dolbyhp"].subscribers)
	}
	if stream, _ := peer.audioTrack.Stream(); stream.ID() != "audio_dolbyhp" {
		t.Fatalf("track stream = %s, want audio_dolbyhp", stream.ID())
	}
}

func TestPeerSetAudioWhilePausedOnlyRemembersTrack(t *testing.T) {
	peer, selector, session := newAudioTestPeer(t)

	id := "dolbyhp"
	if err := peer.SetAudio(types.PeerAudioRequest{ID: &id}); err != nil {
		t.Fatal(err)
	}
	expectAudioSignal(t, session, types.PeerAudio{Disabled: true, ID: "dolbyhp"})

	// paused: no pipeline subscription yet
	if selector.streams["dolbyhp"].subscribers != 0 {
		t.Fatalf("dolbyhp subscribers while paused = %d, want 0", selector.streams["dolbyhp"].subscribers)
	}

	// unpausing subscribes to the selected track, not the default one
	enabled := false
	if err := peer.SetAudio(types.PeerAudioRequest{Disabled: &enabled}); err != nil {
		t.Fatal(err)
	}
	expectAudioSignal(t, session, types.PeerAudio{Disabled: false, ID: "dolbyhp"})
	if selector.streams["dolbyhp"].subscribers != 1 || selector.streams["plii"].subscribers != 0 {
		t.Fatalf("subscribers plii=%d dolbyhp=%d, want 0 and 1",
			selector.streams["plii"].subscribers, selector.streams["dolbyhp"].subscribers)
	}
}

func TestPeerSetAudioUnknownTrack(t *testing.T) {
	peer, _, session := newAudioTestPeer(t)

	id := "missing"
	if err := peer.SetAudio(types.PeerAudioRequest{ID: &id}); err != types.ErrWebRTCStreamNotFound {
		t.Fatalf("err = %v, want ErrWebRTCStreamNotFound", err)
	}
	if got := peer.Audio(); got.ID != "plii" {
		t.Fatalf("Audio().ID = %q, want plii", got.ID)
	}
	select {
	case got := <-session.sent:
		t.Fatalf("unexpected signal %+v", got)
	case <-time.After(100 * time.Millisecond):
	}
}
