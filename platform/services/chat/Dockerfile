# syntax=docker/dockerfile:1
#
# Multi-stage build, mirror of platform/services/directory/Dockerfile:
#
#   - deps     downloads modules once; cached unless go.mod/sum changes.
#   - test     runs `go test ./...` against the current source.
#   - build    produces the static binary.
#   - runtime  final image FROM scratch.
#
# CGO is off so the binary is fully static. The storage layer uses
# modernc.org/sqlite (pure Go SQLite), so the scratch image needs no
# shared libraries.

FROM golang:1.25-alpine AS deps
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download

FROM deps AS test
COPY . .
RUN go test ./... -count=1

FROM deps AS build
COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build \
    -trimpath \
    -ldflags "-s -w" \
    -o /out/chat \
    ./cmd/chat

FROM scratch AS runtime
COPY --from=build /out/chat /chat
ENV CHAT_DB_PATH=/data/chat.db
EXPOSE 8082
ENTRYPOINT ["/chat"]
