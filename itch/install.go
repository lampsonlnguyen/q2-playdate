// SPDX-License-Identifier: GPL-3.0-or-later
package main

import (
	"archive/zip"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"path"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
)

const maxExpanded = 64 << 20

var session context.Context

func transfer(c *http.Client, address, dir string) (*os.File, int64, error) {
	res, err := fetch(c, address, nil, "")
	if err != nil {
		return nil, 0, err
	}
	defer res.Body.Close()
	if res.ContentLength > maxArchive {
		return nil, 0, errors.New("Download exceeds 48 MiB")
	}
	file, err := os.CreateTemp(dir, ".itch-download-")
	if err != nil {
		return nil, 0, err
	}
	if err := os.Remove(file.Name()); err != nil {
		file.Close()
		return nil, 0, err
	}
	size, err := io.Copy(file, io.LimitReader(res.Body, maxArchive+1))
	if err != nil || size > maxArchive {
		file.Close()
		return nil, 0, errors.New("Download incomplete or too large")
	}
	return file, size, nil
}

func safePath(name string) bool {
	if name == "" || len(name) > 240 || path.IsAbs(name) || path.Clean(name) != name {
		return false
	}
	for _, component := range strings.Split(name, "/") {
		if component == "." || component == ".." || strings.TrimRight(component, ". ") != component {
			return false
		}
		for _, r := range component {
			if r < 32 || r == 127 || strings.ContainsRune("<>:\"\\|?*", r) {
				return false
			}
		}
	}
	return true
}

func luaPDZ(file *zip.File) bool {
	if file == nil || file.UncompressedSize64 < 16 || file.UncompressedSize64 > 8<<20 {
		return false
	}
	r, err := file.Open()
	if err != nil {
		return false
	}
	defer r.Close()
	var header [16]byte
	_, err = io.ReadFull(r, header[:])
	return err == nil && string(header[:12]) == "Playdate PDZ" &&
		binary.LittleEndian.Uint32(header[12:])&0x40000000 == 0
}

func installZIP(reader io.ReaderAt, size int64, dir, name string) error {
	archive, err := zip.NewReader(reader, size)
	if err != nil || len(archive.File) > 4096 {
		return errNoBundle
	}
	files := make(map[string]*zip.File)
	folded := make(map[string]bool)
	for _, file := range archive.File {
		entry := strings.TrimSuffix(file.Name, "/")
		if !safePath(entry) {
			return errors.New("Unsafe archive path")
		}
		if file.FileInfo().IsDir() {
			continue
		}
		if !file.Mode().IsRegular() || folded[strings.ToLower(entry)] {
			return errors.New("Unsupported archive entry")
		}
		folded[strings.ToLower(entry)] = true
		files[entry] = file
	}

	var root string
	found := false
	for entry := range files {
		if path.Base(entry) != "pdxinfo" {
			continue
		}
		prefix := strings.TrimSuffix(entry, "pdxinfo")
		if folded[strings.ToLower(prefix+"pdex.bin")] ||
			!luaPDZ(files[prefix+"main.pdz"]) {
			continue
		}
		if found {
			return errors.New("Archive contains multiple Lua bundles")
		}
		root, found = prefix, true
	}
	if !found {
		return errNoBundle
	}

	stage, err := os.MkdirTemp(dir, ".itch-install-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	var expanded uint64
	for entry, file := range files {
		if !strings.HasPrefix(entry, root) {
			continue
		}
		if session.Err() != nil {
			return session.Err()
		}
		if file.UncompressedSize64 > maxExpanded-expanded {
			return errors.New("Bundle exceeds 64 MiB unpacked")
		}
		expanded += file.UncompressedSize64
		dest := filepath.Join(stage, filepath.FromSlash(strings.TrimPrefix(entry, root)))
		if err := os.MkdirAll(filepath.Dir(dest), 0755); err != nil {
			return err
		}
		if err := extract(file, dest); err != nil {
			return err
		}
	}
	if session.Err() != nil {
		return session.Err()
	}
	destination := filepath.Join(dir, name)
	if _, err := os.Lstat(destination); !os.IsNotExist(err) {
		if err == nil {
			return errExists
		}
		return err
	}
	return os.Rename(stage, destination)
}

func extract(file *zip.File, dest string) error {
	source, err := file.Open()
	if err != nil {
		return err
	}
	defer source.Close()
	target, err := os.OpenFile(dest, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644)
	if err != nil {
		return err
	}
	defer target.Close()
	n, err := io.Copy(target, io.LimitReader(source, int64(file.UncompressedSize64)+1))
	if err != nil {
		return err
	}
	if uint64(n) != file.UncompressedSize64 {
		return errors.New("Archive file size mismatch")
	}
	if err := target.Sync(); err != nil {
		return err
	}
	return target.Close()
}

func download(c *http.Client, base, dir string) error {
	data, err := metadata(c, base)
	if err != nil {
		return err
	}
	if !isFree(data.Price) {
		return errors.New("This project requires payment")
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	lock, err := os.OpenFile(filepath.Join(dir, ".itch-lock"), os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return err
	}
	defer lock.Close()
	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		return errors.New("Another installation is in progress")
	}
	title := strings.Map(func(r rune) rune {
		if r < 32 || strings.ContainsRune("<>:\"/\\|?*", r) {
			return ' '
		}
		return r
	}, data.Title)
	title = strings.Trim(clean(title, 92), ". ")
	if title == "" {
		title = "Playdate Game"
	}
	suffix := fmt.Sprintf(" [itch %d].pdx", data.ID)
	existing, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	for _, item := range existing {
		if item.IsDir() && strings.HasSuffix(item.Name(), suffix) {
			return errExists
		}
	}

	files, signed, csrf, key, err := guestUploads(c, base)
	if err != nil {
		return err
	}
	eligible := 0
	lastError := errNoBundle
	for _, file := range files {
		if file.id == "" || !strings.HasSuffix(strings.ToLower(file.name), ".zip") {
			continue
		}
		if eligible == 8 {
			break
		}
		eligible++
		address, err := postURL(c, base+"/file/"+file.id, signed,
			url.Values{"csrf_token": {csrf}, "download_key_id": {key}})
		if err != nil {
			lastError = err
			continue
		}
		staged, size, err := transfer(c, address, dir)
		if err != nil {
			lastError = err
			continue
		}
		err = installZIP(staged, size, dir, title+suffix)
		staged.Close()
		if err == nil || errors.Is(err, errExists) {
			return err
		}
		lastError = err
	}
	return lastError
}

func run(args []string) error {
	c := newClient()
	if len(args) == 2 && args[0] == "list" {
		n, _ := strconv.Atoi(args[1])
		return list(c, n, os.Stdout)
	}
	if len(args) == 3 && args[0] == "download" {
		if u, ok := project(args[1]); ok {
			return download(c, strings.TrimRight(u.String(), "/"), args[2])
		}
	}
	return errVisit
}

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, os.Interrupt)
	defer stop()
	session = ctx
	if os.Getenv("SSL_CERT_FILE") == "" {
		if exe, err := os.Executable(); err == nil {
			certs := filepath.Join(filepath.Dir(exe), "itch-ca.pem")
			if _, err := os.Stat(certs); err == nil {
				_ = os.Setenv("SSL_CERT_FILE", certs)
			}
		}
	}
	if err := run(os.Args[1:]); err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
}
