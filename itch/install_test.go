package main

import (
	"archive/zip"
	"bytes"
	"context"
	"os"
	"path/filepath"
	"testing"
)

func fixture(t *testing.T, entries map[string][]byte) *bytes.Reader {
	t.Helper()
	var data bytes.Buffer
	writer := zip.NewWriter(&data)
	for name, payload := range entries {
		file, err := writer.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := file.Write(payload); err != nil {
			t.Fatal(err)
		}
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	return bytes.NewReader(data.Bytes())
}

func TestInstallZIP(t *testing.T) {
	session = context.Background()
	dir := t.TempDir()
	data := append([]byte("Playdate PDZ"), 0, 0, 0, 0)
	entries := map[string][]byte{
		"Release/Demo.pdx/pdxinfo": []byte("name=Demo"),
		"Release/Demo.pdx/main.pdz": data,
		"Release/Demo.pdx/assets/demo.txt": []byte("asset"),
	}
	archive := fixture(t, entries)
	if err := installZIP(archive, archive.Size(), dir, "Demo.pdx"); err != nil {
		t.Fatal(err)
	}
	if b, err := os.ReadFile(filepath.Join(dir, "Demo.pdx", "assets", "demo.txt")); err != nil || string(b) != "asset" {
		t.Fatalf("asset: %q, %v", b, err)
	}
	if err := installZIP(archive, archive.Size(), dir, "Demo.pdx"); err != errExists {
		t.Fatalf("duplicate: %v", err)
	}
	for name, value := range map[string][]byte{
		"Release/Demo.pdx/pdex.bin": []byte("native"),
		"../escape": []byte("traversal"),
		"Release/Demo.pdx/ASSETS/demo.txt": []byte("case collision"),
	} {
		entries[name] = value
		archive := fixture(t, entries)
		if err := installZIP(archive, archive.Size(), dir, "Rejected.pdx"); err == nil {
			t.Fatalf("accepted %s", name)
		}
		delete(entries, name)
	}
	data[15] = 0x40
	archive = fixture(t, entries)
	if err := installZIP(archive, archive.Size(), dir, "Rejected.pdx"); err == nil {
		t.Fatal("accepted encrypted PDZ")
	}
	if _, err := os.Stat(filepath.Join(dir, "Rejected.pdx")); !os.IsNotExist(err) {
		t.Fatalf("failed installation created a new game: %v", err)
	}
}

func TestCatalog(t *testing.T) {
	const page = "<link rel='next' href='?page=2'><div class='browse_game_grid'>" +
		"<div class='game_cell' data-game_id='7'><a class='title game_link' href='https://demo.itch.io/crank'>Crank Demo</a></div></div>"
	games, more, err := catalogEntries([]byte(page))
	if err != nil || !more || len(games) != 1 || games[0].title != "Crank Demo" {
		t.Fatalf("catalog: %+v, %t, %v", games, more, err)
	}
}
