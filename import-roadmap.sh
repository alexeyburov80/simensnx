#!/bin/bash

# Конфигурация
REPO="alexeyburov80/simensnx"
PROJECT_NUMBER=3
OWNER="alexeyburov80"  # владелец проекта (без /simensnx)

echo "🚀 Начинаем импорт в проект: @alexeyburov80-simensnx (ID: $PROJECT_NUMBER)"
echo "📖 Парсинг ROADMAP.md..."

total=0
created=0
added=0
current_phase=""

while IFS= read -r line; do
    # Определяем фазу
    if [[ "$line" =~ ^"## Phase "[0-9]+ ]]; then
        current_phase="$line"
        current_phase="${current_phase##\#\# }"
        echo ""
        echo "📂 Обработка: $current_phase"
        echo "----------------------------------------"
        continue
    fi
    
    # Находим задачи
    if [[ "$line" =~ ^[[:space:]]*[-*][[:space:]]*\[([ x])\][[:space:]]*(.+) ]]; then
        status="${BASH_REMATCH[1]}"
        task_title="${BASH_REMATCH[2]}"
        total=$((total + 1))
        
        echo "📝 $task_title"
        
        # Формируем тело issue
        body="**Фаза:** $current_phase\n"
        body+="**Статус:** $([ "$status" == "x" ] && echo "✅ Выполнено" || echo "⏳ В работе/ожидает")\n\n"
        body+="**Описание:** $task_title\n\n"
        body+="**Контекст:**\n"
        body+="- Исходный пункт из ROADMAP.md\n"
        body+="- [ ] Реализовать\n\n"
        body+="---\n"
        body+="_Автоматически импортировано из ROADMAP.md_"
        
        phase_num=$(echo "$current_phase" | grep -o "[0-9]" | head -1)
        title="[Phase $phase_num] $task_title"
        
        echo "  ➜ Создаю issue..."
        
        # Создаем issue
        issue_url=$(gh issue create \
            --repo "$REPO" \
            --title "$title" \
            --body "$body" \
            --label "roadmap,phase-$phase_num" 2>&1)
        
        if [[ $? -eq 0 ]]; then
            echo "  ✅ Создано: $issue_url"
            issue_num=$(echo "$issue_url" | grep -o '[0-9]*$')
            created=$((created + 1))
            
            # Добавляем в проект (исправленная команда)
            echo "  ➜ Добавляю в проект..."
            gh project item-add "$PROJECT_NUMBER" \
                --owner "$OWNER" \
                --url "https://github.com/$REPO/issues/$issue_num" 2>&1
            
            if [[ $? -eq 0 ]]; then
                echo "  ✅ Добавлено в проект @alexeyburov80-simensnx"
                added=$((added + 1))
            else
                echo "  ⚠️  Не удалось добавить в проект"
            fi
        else
            echo "  ❌ Ошибка: $issue_url"
        fi
        
        echo ""
        sleep 1
    fi
done < ROADMAP.md

echo "========================================"
echo "📊 Итог импорта:"
echo "   Всего задач: $total"
echo "   Создано issues: $created"
echo "   Добавлено в проект: $added"
echo "========================================"
echo "✅ Готово!"