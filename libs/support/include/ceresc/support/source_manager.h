#pragma once

#include "source_location.h"
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <algorithm>

// SourceManager - owns every source file's full buffer and hands out stable std::string_view
// slices over it.
//
// Tokens and AST nodes never copy their lexeme, they view into this buffer (same convention as
// ceres::casm::Token in CeresASM). Also translates a byte offset back to line/column for
// diagnostics.h. See the architecture plan, §4.
//
// Implemented in Fase 0 of the phased plan (§13).

namespace ceresc::support
{
	class SourceManager;

	class SourceBuffer
	{
	public:
		friend class SourceManager;

	private:
		SourceId _sourceId;
		std::string _name;
		std::string _buffer;
		std::vector<Offset> _lineOffsets;

	public:
		SourceBuffer() = delete;
		SourceBuffer(const SourceBuffer&) = delete;
		SourceBuffer(SourceBuffer&&) = delete;
		~SourceBuffer() noexcept = default;

		SourceBuffer& operator=(const SourceBuffer&) = delete;
		SourceBuffer& operator=(SourceBuffer&&) = delete;

	private:
		SourceBuffer(SourceId sourceId, std::string_view name, std::string&& buffer) noexcept :
			_sourceId(sourceId), _name(name), _buffer(std::move(buffer))
		{
			_lineOffsets.push_back(0);
			for (size_t i = 0; i < _buffer.size(); ++i)
			{
				if (_buffer[i] == '\n')
					_lineOffsets.push_back(static_cast<Offset>(i + 1));
			}
		}

	public:
		constexpr SourceId sourceId() const noexcept { return _sourceId; }
		constexpr std::string_view name() const noexcept { return _name; }
		constexpr std::string_view buffer() const noexcept { return _buffer; }

		[[nodiscard]] SourceLocation getLocation(Offset offset) const noexcept
		{
			auto bufferSize = static_cast<Offset>(_buffer.size());
			if (offset > bufferSize)
				return {};

			auto it = std::upper_bound(_lineOffsets.begin(), _lineOffsets.end(), offset);
			LineNumber line = static_cast<LineNumber>(std::distance(_lineOffsets.begin(), it));
			ColumnNumber column = static_cast<ColumnNumber>(offset - _lineOffsets[line - 1] + 1);
			return { _sourceId, line, column, offset };
		}
	};

	class SourceManager
	{
	private:
		std::vector<std::unique_ptr<SourceBuffer>> _buffers;
		SourceIdGenerator _sourceIdGenerator;

	public:
		SourceManager() = default;
		SourceManager(const SourceManager&) = delete;
		SourceManager(SourceManager&&) = delete;
		~SourceManager() noexcept = default;

		SourceManager& operator=(const SourceManager&) = delete;
		SourceManager& operator=(SourceManager&&) = delete;

	public:
		SourceId registerBuffer(std::string_view name, std::string&& content)
		{
			SourceId sourceId = _sourceIdGenerator.generate();
			_buffers.emplace_back(std::unique_ptr<SourceBuffer>(new SourceBuffer(sourceId, name, std::move(content))));
			return sourceId;
		}

		[[nodiscard]] const SourceBuffer* getBuffer(SourceId sourceId) const noexcept
		{
			if (!sourceId || sourceId.value() > _buffers.size())
				return nullptr;
			return _buffers[sourceId.value() - 1].get();
		}

		[[nodiscard]] SourceLocation getLocation(SourceId sourceId, Offset offset) const noexcept
		{
			const SourceBuffer* buffer = getBuffer(sourceId);
			if (!buffer)
				return {};
			return buffer->getLocation(offset);
		}
	};
}
