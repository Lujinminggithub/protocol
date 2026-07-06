package main
import (
  "fmt"
  cfgpkg "github.com/local/xgw-edge/internal/config"
  core "github.com/local/xgw-edge/internal/coremodel"
)
func main(){
  cfg, err := cfgpkg.LoadServer("examples/server.json")
  if err != nil { panic(err) }
  fc := cfgpkg.DefaultFlowClassifier().ToCore()
  targets := []string{
    "29-courier.push.apple.com:443",
    "13-courier.push.apple.com:443",
    "time.apple.com:123",
    "gdmf.apple.com:443",
    "bag.itunes.apple.com:443",
    "gspe1-ssl.ls.apple.com:443",
    "pagead2.googlesyndication.com:443",
    "ip.sb:443",
  }
  _ = cfg
  for _, t := range targets {
    r := core.ClassifyFlowWithRules(core.FlowKindTCP, t, &fc)
    fmt.Printf("tcp %s => class=%s priority=%s\n", t, r.Class, r.Priority.String())
    r2 := core.ClassifyFlowWithRules(core.FlowKindUDP, t, &fc)
    fmt.Printf("udp %s => class=%s priority=%s\n", t, r2.Class, r2.Priority.String())
  }
}
