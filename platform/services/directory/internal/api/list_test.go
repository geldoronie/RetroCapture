package api

// Tests for GET /streams — public list with sort/search/limit.

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"
)

// regOut is the (id, token) pair we keep so a test can both reference
// a stream by name in subsequent calls and act as its owner.
type regOut struct {
	id, token string
}

// registerNamed creates a stream with a given name and optionally
// seeds its clientCount via a heartbeat. Returns the id + owner
// token so tests can perform owner-gated follow-ups.
func registerNamed(t *testing.T, h http.Handler, name string, clientCount int) regOut {
	t.Helper()
	req := validRegisterReq()
	req.Name = name
	rec, env := doJSON(t, h, "POST", "/register", req)
	if rec.Code != http.StatusCreated {
		t.Fatalf("register %q: %d", name, rec.Code)
	}
	reg := mustData[RegisterResponse](t, env)

	if clientCount > 0 {
		doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
			StreamID: reg.StreamID, OwnerToken: reg.OwnerToken, ClientCount: &clientCount,
		})
	}
	return regOut{id: reg.StreamID, token: reg.OwnerToken}
}

func TestList_Empty(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "GET", "/streams", nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
	list := mustData[ListResponse](t, env)
	if list.TotalCount != 0 || len(list.Streams) != 0 {
		t.Fatalf("expected empty, got %+v", list)
	}
}

func TestList_ReturnsRegistered(t *testing.T) {
	h, _ := newTestServer(t)
	registerNamed(t, h, "Alpha", 0)
	registerNamed(t, h, "Beta", 0)

	rec, env := doJSON(t, h, "GET", "/streams", nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
	list := mustData[ListResponse](t, env)
	if list.TotalCount != 2 || len(list.Streams) != 2 {
		t.Fatalf("totalCount=%d len=%d, want 2/2", list.TotalCount, len(list.Streams))
	}
}

// Default sort is by client_count desc.
func TestList_SortClientsDescIsDefault(t *testing.T) {
	h, _ := newTestServer(t)
	registerNamed(t, h, "low", 1)
	registerNamed(t, h, "high", 50)
	registerNamed(t, h, "mid", 10)

	_, env := doJSON(t, h, "GET", "/streams", nil)
	list := mustData[ListResponse](t, env)
	got := []string{list.Streams[0].Name, list.Streams[1].Name, list.Streams[2].Name}
	want := []string{"high", "mid", "low"}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("position %d: got %q, want %q (full=%v)", i, got[i], want[i], got)
		}
	}
}

func TestList_SortRecent(t *testing.T) {
	h, _ := newTestServer(t)
	// Heartbeat order shapes last_heartbeat_at; register the streams
	// in one order and heartbeat them in another so 'recent' isn't
	// accidentally the same as registration order.
	a := registerNamed(t, h, "a", 0)
	b := registerNamed(t, h, "b", 0)
	c := registerNamed(t, h, "c", 0)

	// Sleep between heartbeats so SQLite's 1-second-resolution
	// timestamps actually differ.
	for _, x := range []regOut{a, c, b} {
		doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{StreamID: x.id, OwnerToken: x.token})
		time.Sleep(1100 * time.Millisecond)
	}

	_, env := doJSON(t, h, "GET", "/streams?sort=recent", nil)
	list := mustData[ListResponse](t, env)
	// Most recent heartbeat first: b (last), c, a.
	want := []string{"b", "c", "a"}
	for i, w := range want {
		if list.Streams[i].Name != w {
			t.Fatalf("position %d: got %q, want %q (full=%+v)", i, list.Streams[i].Name, w, list.Streams)
		}
	}
}

func TestList_SortName(t *testing.T) {
	h, _ := newTestServer(t)
	registerNamed(t, h, "zulu", 0)
	registerNamed(t, h, "alpha", 0)
	registerNamed(t, h, "mike", 0)

	_, env := doJSON(t, h, "GET", "/streams?sort=name", nil)
	list := mustData[ListResponse](t, env)
	want := []string{"alpha", "mike", "zulu"}
	for i, w := range want {
		if list.Streams[i].Name != w {
			t.Fatalf("position %d: got %q, want %q", i, list.Streams[i].Name, w)
		}
	}
}

func TestList_SortInvalid(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "GET", "/streams?sort=bogus", nil)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeInvalidRequest {
		t.Fatalf("error envelope = %+v", env.Error)
	}
}

func TestList_SearchByName(t *testing.T) {
	h, _ := newTestServer(t)
	registerNamed(t, h, "CRT Mattias", 0)
	registerNamed(t, h, "NTSC Easymode", 0)
	registerNamed(t, h, "Plain", 0)

	_, env := doJSON(t, h, "GET", "/streams?q=crt", nil)
	list := mustData[ListResponse](t, env)
	if list.TotalCount != 1 {
		t.Fatalf("totalCount = %d, want 1", list.TotalCount)
	}
	if len(list.Streams) != 1 || list.Streams[0].Name != "CRT Mattias" {
		t.Fatalf("unexpected match: %+v", list.Streams)
	}
}

func TestList_SearchByNickname(t *testing.T) {
	h, _ := newTestServer(t)
	// Register one stream with a recognisable nickname.
	req := validRegisterReq()
	req.Name = "Generic stream"
	req.HostNickname = "crt-fan"
	doJSON(t, h, "POST", "/register", req)

	// And a decoy that doesn't match.
	req2 := validRegisterReq()
	req2.Name = "Other"
	req2.HostNickname = "someone"
	doJSON(t, h, "POST", "/register", req2)

	_, env := doJSON(t, h, "GET", "/streams?q=crt", nil)
	list := mustData[ListResponse](t, env)
	if list.TotalCount != 1 {
		t.Fatalf("totalCount = %d, want 1", list.TotalCount)
	}
	if len(list.Streams) != 1 || list.Streams[0].HostNickname != "crt-fan" {
		t.Fatalf("unexpected match: %+v", list.Streams)
	}
}

func TestList_LimitParamRespected(t *testing.T) {
	h, _ := newTestServer(t)
	for i := 0; i < 5; i++ {
		registerNamed(t, h, "s", 0)
	}
	_, env := doJSON(t, h, "GET", "/streams?limit=2", nil)
	list := mustData[ListResponse](t, env)
	if list.TotalCount != 5 {
		t.Fatalf("totalCount = %d, want 5 (pre-limit count)", list.TotalCount)
	}
	if len(list.Streams) != 2 {
		t.Fatalf("returned %d streams, want 2 after limit", len(list.Streams))
	}
}

func TestList_LimitBadValue(t *testing.T) {
	h, _ := newTestServer(t)
	cases := []string{"0", "-1", "501", "abc"}
	for _, v := range cases {
		t.Run(v, func(t *testing.T) {
			rec, env := doJSON(t, h, "GET", "/streams?limit="+v, nil)
			if rec.Code != http.StatusBadRequest {
				t.Fatalf("limit=%q status = %d, want 400", v, rec.Code)
			}
			if env.Error == nil || env.Error.Code != CodeInvalidRequest {
				t.Fatalf("envelope = %+v", env.Error)
			}
		})
	}
}

// owner_token must never appear in any field of any entry. This is a
// stronger version of the existing leak guard.
func TestList_OwnerTokenNeverInResponse(t *testing.T) {
	h, _ := newTestServer(t)
	registerNamed(t, h, "a", 0)
	registerNamed(t, h, "b", 0)
	_, env := doJSON(t, h, "GET", "/streams", nil)
	b, _ := json.Marshal(env)
	if strings.Contains(string(b), "ownerToken") || strings.Contains(string(b), "owner_token") {
		t.Fatal("list response contains owner token field")
	}
}
