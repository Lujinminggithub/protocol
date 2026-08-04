package webapp

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"sync"
)

type storedDeviceSecret struct {
	Password string `json:"password"`
}

type deviceSecretStore struct {
	path string
	mu   sync.Mutex
}

func (s *deviceSecretStore) configured() bool { return s != nil && s.path != "" }

func (s *deviceSecretStore) update(ref, password string) error {
	if !s.configured() {
		return errors.New("device password storage is not configured")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	values, err := s.read()
	if err != nil {
		return err
	}
	encoded, err := json.Marshal(storedDeviceSecret{Password: password})
	if err != nil {
		return err
	}
	values[ref] = encoded
	return s.write(values)
}

func (s *deviceSecretStore) delete(ref string) error {
	if !s.configured() || ref == "" {
		return nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	values, err := s.read()
	if err != nil {
		return err
	}
	if _, ok := values[ref]; !ok {
		return nil
	}
	delete(values, ref)
	return s.write(values)
}

func (s *deviceSecretStore) read() (map[string]json.RawMessage, error) {
	values := map[string]json.RawMessage{}
	data, err := os.ReadFile(s.path)
	if errors.Is(err, os.ErrNotExist) {
		return values, nil
	}
	if err != nil {
		return nil, err
	}
	if len(data) == 0 {
		return values, nil
	}
	if err = json.Unmarshal(data, &values); err != nil {
		return nil, errors.New("device password store is invalid")
	}
	return values, nil
}

func (s *deviceSecretStore) write(values map[string]json.RawMessage) (err error) {
	data, err := json.MarshalIndent(values, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	directory := filepath.Dir(s.path)
	if err = os.MkdirAll(directory, 0700); err != nil {
		return err
	}
	temporary, err := os.CreateTemp(directory, ".device-secrets.new-*")
	if err != nil {
		return err
	}
	temporaryPath := temporary.Name()
	defer func() {
		_ = temporary.Close()
		if err != nil {
			_ = os.Remove(temporaryPath)
		}
	}()
	if err = temporary.Chmod(0600); err != nil {
		return err
	}
	if _, err = temporary.Write(data); err != nil {
		return err
	}
	if err = temporary.Sync(); err != nil {
		return err
	}
	if err = temporary.Close(); err != nil {
		return err
	}
	if err = os.Rename(temporaryPath, s.path); err != nil && runtime.GOOS == "windows" {
		if removeErr := os.Remove(s.path); removeErr != nil && !errors.Is(removeErr, os.ErrNotExist) {
			return removeErr
		}
		err = os.Rename(temporaryPath, s.path)
	}
	return err
}
