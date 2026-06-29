module github.com/local/xgw-edge

go 1.26

require (
	github.com/apernet/hysteria/core/v2 v2.0.0-00010101000000-000000000000
	github.com/quic-go/quic-go v0.60.0
	go.uber.org/zap v1.27.1
	golang.org/x/sys v0.45.0
)

require (
	fyne.io/fyne/v2 v2.7.4 // indirect
	github.com/apernet/quic-go v0.59.1-0.20260425001925-6c6cc9bcb716 // indirect
	github.com/davecgh/go-spew v1.1.1 // indirect
	github.com/lxn/walk v0.0.0-20210112085537-c389da54e794 // indirect
	github.com/lxn/win v0.0.0-20210218163916-a377121e959e // indirect
	github.com/pmezard/go-difflib v1.0.0 // indirect
	github.com/quic-go/qpack v0.6.0 // indirect
	github.com/stretchr/objx v0.5.2 // indirect
	github.com/stretchr/testify v1.11.1 // indirect
	go.uber.org/multierr v1.10.0 // indirect
	golang.org/x/crypto v0.51.0 // indirect
	golang.org/x/exp v0.0.0-20240506185415-9bf2ced13842 // indirect
	golang.org/x/net v0.55.0 // indirect
	golang.org/x/text v0.37.0 // indirect
	gopkg.in/Knetic/govaluate.v3 v3.0.0 // indirect
	gopkg.in/yaml.v3 v3.0.1 // indirect
)

replace github.com/apernet/hysteria/core/v2 => ./third_party/hysteria-core
replace github.com/lxn/walk => ./third_party/walk
replace github.com/lxn/walk => ./third_party/walk
