package ratelimit

import (
	"sync"
	"testing"
	"time"
)

func TestPerHour(t *testing.T) {
	c := PerHour(3600) // 1 per second sustained, burst 3600
	if c.Rate != 1.0 {
		t.Fatalf("rate = %f, want 1.0", c.Rate)
	}
	if c.Capacity != 3600.0 {
		t.Fatalf("capacity = %f, want 3600", c.Capacity)
	}
}

func TestAllowsUpToCapacity(t *testing.T) {
	// Tight config: 5 capacity, slow refill — so we can hit the
	// burst limit without waiting.
	l := New(Config{Rate: 0.0001, Capacity: 5})
	for i := 0; i < 5; i++ {
		ok, ra := l.Allow("x")
		if !ok {
			t.Fatalf("request %d denied unexpectedly (retryAfter=%v)", i, ra)
		}
	}
	ok, ra := l.Allow("x")
	if ok {
		t.Fatal("6th request should have been denied")
	}
	if ra <= 0 {
		t.Fatalf("retryAfter should be > 0 on denial, got %v", ra)
	}
}

func TestBucketRefills(t *testing.T) {
	// 10 tokens/sec, capacity 2. After exhausting both, we expect a
	// refill of at least one token within ~100ms.
	l := New(Config{Rate: 10, Capacity: 2})
	if ok, _ := l.Allow("a"); !ok {
		t.Fatal("first request should pass")
	}
	if ok, _ := l.Allow("a"); !ok {
		t.Fatal("second request should pass")
	}
	if ok, _ := l.Allow("a"); ok {
		t.Fatal("third request should fail (burst exhausted)")
	}
	time.Sleep(200 * time.Millisecond)
	if ok, _ := l.Allow("a"); !ok {
		t.Fatal("after refill the next request should pass")
	}
}

func TestKeysAreIndependent(t *testing.T) {
	// Two clients shouldn't be able to starve each other.
	l := New(Config{Rate: 0.0001, Capacity: 1})
	if ok, _ := l.Allow("a"); !ok {
		t.Fatal("a's first request should pass")
	}
	if ok, _ := l.Allow("b"); !ok {
		t.Fatal("b's first request should pass (b has its own bucket)")
	}
	if ok, _ := l.Allow("a"); ok {
		t.Fatal("a's second request should fail")
	}
	if ok, _ := l.Allow("b"); ok {
		t.Fatal("b's second request should fail")
	}
}

func TestEvictStale(t *testing.T) {
	l := New(Config{Rate: 1, Capacity: 5})
	_, _ = l.Allow("a")
	_, _ = l.Allow("b")
	if l.Size() != 2 {
		t.Fatalf("size = %d, want 2", l.Size())
	}
	// 0-duration idle means "evict everything not touched this very
	// instant". Touch advances each Allow, so a tiny sleep makes
	// both stale.
	time.Sleep(20 * time.Millisecond)
	n := l.EvictStale(10 * time.Millisecond)
	if n != 2 {
		t.Fatalf("evicted = %d, want 2", n)
	}
	if l.Size() != 0 {
		t.Fatalf("size after evict = %d, want 0", l.Size())
	}
}

func TestRetryAfterScales(t *testing.T) {
	// With Rate=1/sec and an empty bucket, retryAfter for one more
	// token should be ~1s.
	l := New(Config{Rate: 1, Capacity: 1})
	_, _ = l.Allow("k") // burns the lone token
	ok, ra := l.Allow("k")
	if ok {
		t.Fatal("should be denied")
	}
	// Tolerate jitter — anything in [700ms, 1.5s] is reasonable.
	if ra < 700*time.Millisecond || ra > 1500*time.Millisecond {
		t.Fatalf("retryAfter = %v, want roughly 1s", ra)
	}
}

func TestConcurrentSafe(t *testing.T) {
	// Loosely tests for races. Run with `go test -race` to actually
	// catch them.
	l := New(Config{Rate: 1000, Capacity: 100})
	var wg sync.WaitGroup
	for i := 0; i < 100; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			for j := 0; j < 100; j++ {
				l.Allow("client")
			}
		}(i)
	}
	wg.Wait()
}
