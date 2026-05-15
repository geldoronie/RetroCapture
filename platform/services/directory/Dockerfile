# syntax=docker/dockerfile:1
#
# Two-stage build: compile in golang:alpine, ship a static binary on
# a scratch base. ~10 MB final image, no shell, no package manager,
# minimal attack surface.
#
# CGO is disabled so the binary is fully static. SQLite support will
# require switching to `modernc.org/sqlite` (pure-Go) rather than
# `mattn/go-sqlite3` (cgo) when storage lands — keeping CGO=0 here
# makes that constraint explicit from day one.

FROM golang:1.23-alpine AS build
WORKDIR /src

# Cache the module graph separately from source so changes to .go
# files don't invalidate the dependency download layer.
COPY go.mod go.sum* ./
RUN go mod download

COPY . .

# Strip debug symbols & DWARF (-s -w) and ensure a fully static
# binary. -trimpath removes the build host's paths from the binary.
RUN CGO_ENABLED=0 GOOS=linux go build \
    -trimpath \
    -ldflags "-s -w" \
    -o /out/directory \
    ./cmd/directory

FROM scratch
COPY --from=build /out/directory /directory
# data/ is bind-mounted by docker-compose; we expose the default path
# as documentation, the runtime creates the dir if missing.
ENV DIRECTORY_DB_PATH=/data/directory.db
EXPOSE 8081
ENTRYPOINT ["/directory"]
