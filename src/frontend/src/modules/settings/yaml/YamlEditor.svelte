<script lang="ts">
  import { history, historyKeymap } from "@codemirror/commands";
  import { yaml } from "@codemirror/lang-yaml";
  import {
    bracketMatching,
    HighlightStyle,
    indentOnInput,
    indentUnit,
    syntaxHighlighting,
  } from "@codemirror/language";
  import { Compartment, EditorState, RangeSet, Text, type Range } from "@codemirror/state";
  import {
    Decoration,
    drawSelection,
    EditorView,
    GutterMarker,
    highlightActiveLine,
    highlightActiveLineGutter,
    keymap,
    lineNumberMarkers,
    lineNumbers,
  } from "@codemirror/view";
  import { tags } from "@lezer/highlight";
  import { onMount } from "svelte";

  import { t } from "../../../data/locale.svelte";
  import { hideFor, show } from "../../../lib/tooltip/tooltip.svelte";

  import type { YamlConfigError } from "./config";

  let {
    value = $bindable(""),
    disabled = false,
    errors = [],
  }: {
    value?: string;
    disabled?: boolean;
    errors?: YamlConfigError[];
  } = $props();

  let host: HTMLDivElement;
  let view = $state.raw<EditorView>();
  let tooltipAnchor: HTMLElement | undefined;
  let lineErrors = new Map<number, string[]>();
  function hideErrorTooltip() {
    if (tooltipAnchor) hideFor(tooltipAnchor);
    tooltipAnchor = undefined;
  }
  const diagnostics = new Compartment();
  const editable = new Compartment();

  class ErrorMarker extends GutterMarker {
    elementClass = "cm-yaml-error-gutter";
  }

  function errorRange(error: YamlConfigError) {
    if (!view) return;
    const line = view.state.doc.line(Math.min(error.line ?? 1, view.state.doc.lines));
    // Use line/column coordinates: pasted CRLF may remain in a document whose
    // configured separator is LF, so globally stripping CR shifts highlights.
    const from = Math.min(line.to, line.from + Math.max(0, (error.column ?? 1) - 1));
    let end = from + 1;
    if (error.to !== undefined) {
      const prefix = value.slice(0, error.to).split(/\r\n|\r|\n/);
      const endLine = view.state.doc.line(Math.min(prefix.length, view.state.doc.lines));
      end = Math.min(endLine.to, endLine.from + prefix[prefix.length - 1].length);
    }
    return { from, to: Math.min(view.state.doc.length, Math.max(from + 1, end)) };
  }

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

  onMount(() => {
    const editor = new EditorView({
      parent: host,
      state: EditorState.create({
        doc: value,
        extensions: [
          lineNumbers({
            domEventHandlers: {
              mouseover: (_view, line, event) => {
                const anchor =
                  event.target instanceof HTMLElement
                    ? event.target.closest<HTMLElement>(".cm-yaml-error-gutter")
                    : null;
                const messages = lineErrors.get(line.from);
                if (anchor && messages) {
                  tooltipAnchor = anchor;
                  void show(anchor, messages.join("\n"));
                }
                return false;
              },
              mouseout: () => {
                hideErrorTooltip();
                return false;
              },
            },
          }),
          diagnostics.of([]),
          highlightActiveLineGutter(),
          highlightActiveLine(),
          drawSelection(),
          history(),
          yaml(),
          indentUnit.of("  "),
          indentOnInput(),
          bracketMatching(),
          syntaxHighlighting(highlightStyle),
          EditorView.lineWrapping,
          EditorState.tabSize.of(2),
          lineEndings.of(EditorState.lineSeparator.of(lineSeparator(value))),
          keymap.of(historyKeymap),
          editable.of([EditorState.readOnly.of(disabled), EditorView.editable.of(!disabled)]),
          language.of([
            EditorView.contentAttributes.of({
              "aria-label": t("YAML configuration"),
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
            "&.cm-focused": { outline: "none" },
            ".cm-scroller": {
              overflow: "visible",
              fontFamily: "ui-monospace, SFMono-Regular, Consolas, monospace",
              lineHeight: "1.6",
              minHeight: "calc(100dvh - 12rem)",
            },
            ".cm-content": { padding: "0", caretColor: "var(--text)" },
            ".cm-line": { padding: "0 0.75rem" },
            ".cm-gutters": {
              backgroundColor: "var(--bg-light)",
              color: "var(--text-2)",
              borderColor: "var(--bg-light-extra)",
            },
            ".cm-activeLine, .cm-activeLineGutter": {
              backgroundColor: "color-mix(in srgb, var(--accent) 8%, transparent)",
            },
            ".cm-yaml-error-line": {
              backgroundColor: "color-mix(in srgb, var(--red) 8%, transparent)",
            },
            ".cm-yaml-error-mark": {
              textDecoration: "underline wavy var(--red)",
              textUnderlineOffset: "3px",
            },
            ".cm-yaml-error-gutter": {
              color: "var(--red)",
              boxShadow: "inset 3px 0 var(--red)",
            },
            ".cm-cursor, .cm-dropCursor": { borderLeftColor: "var(--text)" },
            "&.cm-focused .cm-selectionBackground, .cm-selectionBackground, .cm-content ::selection":
              { backgroundColor: "color-mix(in srgb, var(--accent) 25%, transparent)" },
          }),
        ],
      }),
    });
    view = editor;
    return () => {
      hideErrorTooltip();
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
    if (!view) return;
    hideErrorTooltip();
    lineErrors = new Map();
    if (!errors.length) {
      view.dispatch({ effects: diagnostics.reconfigure([]) });
      return;
    }
    const decorations: Range<Decoration>[] = [];
    for (const error of errors) {
      const range = errorRange(error);
      if (!range) continue;
      const line = view.state.doc.lineAt(range.from);
      const messages = lineErrors.get(line.from) ?? [];
      if (!messages.includes(t(error.message))) messages.push(t(error.message));
      lineErrors.set(line.from, messages);
      if (range.to > range.from) {
        decorations.push(
          Decoration.mark({ class: "cm-yaml-error-mark" }).range(range.from, range.to),
        );
      }
    }
    for (const from of lineErrors.keys()) {
      decorations.push(
        Decoration.line({ attributes: { class: "cm-yaml-error-line" } }).range(from),
      );
    }
    view.dispatch({
      effects: diagnostics.reconfigure([
        EditorView.decorations.of(Decoration.set(decorations, true)),
        lineNumberMarkers.of(
          RangeSet.of(
            [...lineErrors.keys()].map((from) => new ErrorMarker().range(from)),
            true,
          ),
        ),
        EditorView.contentAttributes.of({
          "aria-invalid": "true",
          "aria-description": [...new Set(errors.map((error) => t(error.message)))].join("\n"),
        }),
      ]),
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
        EditorView.contentAttributes.of({
          "aria-label": t("YAML configuration"),
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
  }
</style>
