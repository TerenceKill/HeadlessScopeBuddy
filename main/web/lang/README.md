## Englisch
### Adding Further Languages

To add a new language (e.g., French `fr`) to the web UI:

1. **Create the translation file:** Copy `main/web/lang/en.json` to `main/web/lang/fr.json` (saved as UTF-8 without BOM) and translate all values, `examples`, `cText`, and `cPatterns` entries.
2. **Embed the file in CMake:** Add `"web/lang/fr.json"` to `EMBED_TXTFILES` in `main/CMakeLists.txt`.
3. **Serve the file in `main/web_server.c`:** Declare the `_binary_fr_json_start` / `_binary_fr_json_end` symbols and return them in `lang_handler()` when `/lang/fr.json` is requested.
4. **Register the language in `main/web/index.html`:** Add `'fr'` to `supportedLangs` and add `<option value="fr">FR</option>` inside `<select id="langSelect">`.
5. **Rebuild and flash:** Run `idf.py build flash`.

## Deutsch
### Weitere Sprachen hinzufügen

Um eine zusätzliche Sprache (z. B. Französisch `fr`) in die Weboberfläche aufzunehmen, sind folgende Schritte nötig:

1. **Sprachdatei anlegen:** Kopiere `main/web/lang/en.json` nach `main/web/lang/fr.json` (als UTF-8 ohne BOM speichern) und übersetze alle Texte sowie die Einträge unter `examples`, `cText` und `cPatterns`.
2. **In CMake einbetten:** Ergänze `"web/lang/fr.json"` in `main/CMakeLists.txt` unter `EMBED_TXTFILES`.
3. **Route im Webserver ergänzen:** Binde in `main/web_server.c` die Symbole `_binary_fr_json_start` / `_binary_fr_json_end` ein und liefere sie in `lang_handler()` bei Aufruf von `/lang/fr.json` aus.
4. **Im Frontend registrieren:** Trage `'fr'` in `main/web/index.html` in das Array `supportedLangs` ein und ergänze `<option value="fr">FR</option>` im `<select id="langSelect">`.
5. **Neu bauen und flashen:** Führe `idf.py build flash` aus.
