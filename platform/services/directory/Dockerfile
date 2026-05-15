# syntax=docker/dockerfile:1
#
# Multi-stage build:
#
#   - `deps`   downloads modules once; cached unless go.mod/sum changes.
#   - `test`   runs `go test ./...` against the current source. Used by
#              `docker compose run --rm test` and by CI. Failing tests
#              fail the build of this stage.
#   - `build`  produces the static binary.
#   - `runtime` final image FROM scratch.
#
# CGO is disabled so the binary is fully static (the storage layer
# uses modernc.org/sqlite, which is pure Go, so no SQLite shared
# library is needed).

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
    -o /out/directory \
    ./cmd/directory

FROM scratch AS runtime
COPY --from=build /out/directory /directory
ENV DIRECTORY_DB_PATH=/data/directory.db
EXPOSE 8081
ENTRYPOINT ["/directory"]
