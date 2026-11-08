<script lang="ts">
  import { defaultKeymap, history, historyKeymap, indentWithTab } from "@codemirror/commands";
  import { yaml } from "@codemirror/lang-yaml";
  import {
    bracketMatching,
    HighlightStyle,
    indentOnInput,
    indentUnit,
    syntaxHighlighting,
  } from "@codemirror/language";
  import { highlightSelectionMatches, search, searchKeymap } from "@codemirror/search";
  import { Compartment, EditorState, Text } from "@codemirror/state";
  import {
    drawSelection,
    EditorView,
    highlightActiveLine,
    highlightActiveLineGutter,
    keymap,
    lineNumbers,
  } from "@codemirror/view";
  import { tags } from "@lezer/highlight";
  import { onMount } from "svelte";

  import { t } from "../../../data/locale.svelte";

  let {
    value = $bindable(""),
    disabled = false,
    onSave,
  }: {
    value?: string;
    disabled?: boolean;
    onSave: () => void;
  } = $props();

  let host: HTMLDivElement;
  let view = $state.raw<EditorView>();
  const editable = new Compartment();
  const language = new Compartment();
  const lineEndings = new Compartment();
  const lineSeparator = (text: string) => (text.includes("\r\n") ? "\r\n" : "\n");
  const highlightStyle = HighlightStyle.define([
    { tag: tags.propertyName, color: "var(--blue-light-extra)" },
    { tag: tags.string, color: "var(--green)" },
    { tag: [tags.number, tags.bool, tags.null], color: "var(--yellow-bright)" },
    { tag: tags.comment, color: "var(--text-2)", fontStyle: "italic" },
    { tag: [tags.keyword, tags.meta], color: "var(--violet-light)" },
    { tag: tags.punctuation, color: "var(--text-2)" },
    { tag: tags.invalid, color: "var(--red)" },
  ]);
  const phrases = $derived({
    Find: t("Find"),
    Replace: t("Replace"),
    next: t("next"),
    previous: t("previous"),
    all: t("all"),
    "match case": t("match case"),
    "by word": t("by word"),
    regexp: t("regexp"),
    replace: t("replace"),
    "replace all": t("replace all"),
    close: t("close"),
    "No matches found": t("No matches found"),
    "$ matches": t("$ matches"),
    "Go to line": t("Go to line"),
    go: t("go"),
  });

  onMount(() => {
    const editor = new EditorView({
      parent: host,
      state: EditorState.create({
        doc: value,
        extensions: [
          lineNumbers(),
          highlightActiveLineGutter(),
          highlightActiveLine(),
          drawSelection(),
          history(),
          yaml(),
          indentUnit.of("  "),
          indentOnInput(),
          bracketMatching(),
          syntaxHighlighting(highlightStyle),
          search({ top: true }),
          highlightSelectionMatches(),
          EditorState.tabSize.of(2),
          lineEndings.of(EditorState.lineSeparator.of(lineSeparator(value))),
          keymap.of([
            {
              key: "Mod-s",
              stopPropagation: true,
              run: () => {
                onSave();
                return true;
              },
            },
            // Escape then Tab leaves the editor instead of trapping keyboard focus.
            {
              key: "Escape",
              run: (editor) => {
                editor.setTabFocusMode(2000);
                return false;
              },
            },
            indentWithTab,
            ...defaultKeymap,
            ...historyKeymap,
            ...searchKeymap,
          ]),
          editable.of([EditorState.readOnly.of(disabled), EditorView.editable.of(!disabled)]),
          language.of([
            EditorState.phrases.of(phrases),
            EditorView.contentAttributes.of({
              "aria-label": t("YAML configuration"),
              "aria-describedby": "yaml-editor-help",
              "aria-multiline": "true",
              spellcheck: "false",
              autocorrect: "off",
              autocapitalize: "off",
            }),
          ]),
          EditorView.updateListener.of((update) => {
            if (update.docChanged) value = update.state.sliceDoc();
          }),
          EditorView.theme({
            "&": { backgroundColor: "var(--bg-dark)", color: "var(--text)", fontSize: "0.875rem" },
            "&.cm-focused": { outline: "2px solid var(--accent)", outlineOffset: "-2px" },
            ".cm-scroller": {
              overflow: "auto",
              fontFamily: "ui-monospace, SFMono-Regular, Consolas, monospace",
              lineHeight: "1.6",
              minHeight: "22rem",
              maxHeight: "65vh",
            },
            ".cm-content": { padding: "0.75rem 0", caretColor: "var(--text)" },
            ".cm-line": { padding: "0 0.75rem" },
            ".cm-gutters": {
              backgroundColor: "var(--bg-light)",
              color: "var(--text-2)",
              borderColor: "var(--bg-light-extra)",
            },
            ".cm-activeLine, .cm-activeLineGutter": {
              backgroundColor: "color-mix(in srgb, var(--accent) 8%, transparent)",
            },
            ".cm-cursor, .cm-dropCursor": { borderLeftColor: "var(--text)" },
            "&.cm-focused .cm-selectionBackground, .cm-selectionBackground, .cm-content ::selection":
              { backgroundColor: "color-mix(in srgb, var(--accent) 25%, transparent)" },
            ".cm-panels": {
              backgroundColor: "var(--bg-light)",
              color: "var(--text)",
              borderColor: "var(--bg-light-extra)",
            },
            ".cm-textfield, .cm-button": {
              background: "var(--bg-dark)",
              color: "var(--text)",
              border: "1px solid var(--bg-light-extra)",
              borderRadius: "0.25rem",
            },
            ".cm-searchMatch": {
              backgroundColor: "color-mix(in srgb, var(--accent) 25%, transparent)",
            },
            ".cm-searchMatch.cm-searchMatch-selected": {
              backgroundColor: "color-mix(in srgb, var(--accent) 45%, transparent)",
            },
          }),
        ],
      }),
    });
    view = editor;
    return () => {
      editor.destroy();
      view = undefined;
    };
  });

  $effect(() => {
    if (!view || view.state.sliceDoc() === value) return;
    view.dispatch({
      effects: lineEndings.reconfigure(EditorState.lineSeparator.of(lineSeparator(value))),
      changes: { from: 0, to: view.state.doc.length, insert: Text.of(value.split(/\r\n|\r|\n/)) },
    });
  });
  $effect(() => {
    view?.dispatch({
      effects: editable.reconfigure([
        EditorState.readOnly.of(disabled),
        EditorView.editable.of(!disabled),
      ]),
    });
  });
  $effect(() => {
    view?.dispatch({
      effects: language.reconfigure([
        EditorState.phrases.of(phrases),
        EditorView.contentAttributes.of({
          "aria-label": t("YAML configuration"),
          "aria-describedby": "yaml-editor-help",
          "aria-multiline": "true",
          spellcheck: "false",
          autocorrect: "off",
          autocapitalize: "off",
        }),
      ]),
    });
  });
</script>

<div bind:this={host} class="yaml-editor"></div>

<style>
  .yaml-editor {
    min-width: 0;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    overflow: hidden;
  }
</style>
