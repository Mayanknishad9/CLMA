name: Pull Request
description: Submit changes to CLMA
labels: []
body:
  - type: markdown
    attributes:
      value: |
        Thanks for contributing! Please fill out this form to help us review your PR quickly.

  - type: input
    id: related_issue
    attributes:
      label: Related Issue
      description: Link any related issues (e.g., "Fixes #123" or "Part of #456")
    validations:
      required: false

  - type: textarea
    id: description
    attributes:
      label: Description
      description: What does this PR do? What problem does it solve?
      placeholder: "This PR adds true tree topology to AAN mode, replacing the previous parallel-only fallback..."
    validations:
      required: true

  - type: textarea
    id: changelist
    attributes:
      label: Changes
      description: Summarize the key changes
      placeholder: |
        - `core.py`: Added `_aan_execute_tree` with recursive binary decomposition
        - `core.py`: Added cancellation support to all AAN executors
        - `tests/test_aan.py`: 28 new test cases
    validations:
      required: true

  - type: dropdown
    id: type
    attributes:
      label: Type of Change
      multiple: true
      options:
        - 🐛 Bug fix
        - ✨ New feature
        - 📝 Documentation
        - 🧪 Test
        - 🔧 Refactor
        - ⚡ Performance
        - 🔨 CI/CD
    validations:
      required: true

  - type: textarea
    id: testing
    attributes:
      label: Testing
      description: How did you test these changes?
      placeholder: "Ran 28 new AAN tests + full test suite — all passing."
    validations:
      required: true

  - type: checkboxes
    id: checklist
    attributes:
      label: Pre-Submit Checklist
      options:
        - label: My code follows the project's style guidelines
          required: true
        - label: I have added tests that cover my changes
          required: true
        - label: All existing and new tests pass
          required: true
        - label: I have updated documentation where applicable
          required: false
